/* A-worker IPC: AF_UNIX SOCK_SEQPACKET link over osmo_io (see the description in aworker_ipc_link.h) */
/*
 * SPDX-License-Identifier: AGPL-3.0+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/osmo_io.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>

#define AWORKER_IPC_DEFAULT_TXQUEUE_MAX	1024

struct aworker_ipc_link {
	struct osmo_io_fd *iofd;
	enum aworker_ipc_role role;
	uint16_t worker_idx;
	int log_cat;
	char ident[AWORKER_IPC_MAX_IDENT + 1];
	const struct aworker_ipc_link_ops *ops;
	void *priv;

	enum aworker_ipc_link_state state;
	uint32_t rx_next_seq;		/* expected seq of the next received record */
	uint32_t last_queued_seq;	/* seq of the last record handed to osmo_io */
	uint32_t last_written_seq;	/* seq of the last record fully accepted by the kernel */
	uint32_t rx_records;
	uint32_t tx_queued;
	uint32_t tx_written;

	/* Re-entrancy protection: user call-backs may free the link; the actual free is deferred until the
	 * outermost call-back returns. */
	unsigned int cb_depth;
	bool free_pending;
};

#define LOG_LINK(link, lvl, fmt, args...) \
	LOGP((link)->log_cat, lvl, "aworker-ipc(%s idx=%u): " fmt, \
	     (link)->role == AWORKER_IPC_ROLE_MAIN ? "main" : "worker", (link)->worker_idx, ## args)

static const struct value_string close_reason_names[] = {
	{ AWORKER_IPC_CLOSE_LOCAL, "local" },
	{ AWORKER_IPC_CLOSE_PEER_EOF, "peer-eof" },
	{ AWORKER_IPC_CLOSE_RX_ERROR, "rx-error" },
	{ AWORKER_IPC_CLOSE_RX_OVERSIZE, "rx-oversize" },
	{ AWORKER_IPC_CLOSE_PROTOCOL, "protocol" },
	{ AWORKER_IPC_CLOSE_BAD_SEQ, "bad-seq" },
	{ AWORKER_IPC_CLOSE_NO_HELLO, "no-hello" },
	{ AWORKER_IPC_CLOSE_DUP_HELLO, "dup-hello" },
	{ AWORKER_IPC_CLOSE_INCOMPATIBLE, "incompatible" },
	{ AWORKER_IPC_CLOSE_TX_ERROR, "tx-error" },
	{ AWORKER_IPC_CLOSE_TX_PARTIAL, "tx-partial" },
	{ AWORKER_IPC_CLOSE_TX_SEQ, "tx-seq" },
	{ AWORKER_IPC_CLOSE_TX_QUEUE_FULL, "tx-queue-full" },
	{ AWORKER_IPC_CLOSE_NO_MEMORY, "no-memory" },
	{ 0, NULL }
};

const char *aworker_ipc_close_reason_name(enum aworker_ipc_close_reason reason)
{
	return get_value_string(close_reason_names, reason);
}

static enum aworker_ipc_role peer_role(const struct aworker_ipc_link *link)
{
	return link->role == AWORKER_IPC_ROLE_MAIN ? AWORKER_IPC_ROLE_WORKER : AWORKER_IPC_ROLE_MAIN;
}

static void link_destroy(struct aworker_ipc_link *link)
{
	/* osmo_iofd_free() defers the free if we are inside one of its call-backs */
	osmo_iofd_free(link->iofd);
	link->iofd = NULL;
	talloc_free(link);
}

static void link_enter(struct aworker_ipc_link *link)
{
	link->cb_depth++;
}

/* May free the link. */
static void link_leave(struct aworker_ipc_link *link)
{
	OSMO_ASSERT(link->cb_depth > 0);
	if (--link->cb_depth == 0 && link->free_pending)
		link_destroy(link);
}

/* Close the link and report it. The link must not be used by the caller afterwards (it may be freed). */
static void link_fail(struct aworker_ipc_link *link, enum aworker_ipc_close_reason reason, int err)
{
	struct aworker_ipc_link_close_info info;

	if (link->state == AWORKER_IPC_LINK_CLOSED)
		return;
	link->state = AWORKER_IPC_LINK_CLOSED;

	LOG_LINK(link, reason == AWORKER_IPC_CLOSE_LOCAL ? LOGL_INFO : LOGL_ERROR,
		 "closing link: %s (%d), last written seq %u, last queued seq %u\n",
		 aworker_ipc_close_reason_name(reason), err, link->last_written_seq, link->last_queued_seq);

	/* Closes the socket and discards the transmit queue: nothing queued, and in particular no remainder of a
	 * partially sent record, can be sent after this point. */
	osmo_iofd_close(link->iofd);

	info = (struct aworker_ipc_link_close_info){
		.reason = reason,
		.err = err,
		.last_written_seq = link->last_written_seq,
		.last_queued_seq = link->last_queued_seq,
	};
	link_enter(link);
	link->ops->closed_cb(link, &info);
	link_leave(link);
}

/* Assign the next sequence number and hand the record to osmo_io. Always takes ownership of msg. */
static int queue_record(struct aworker_ipc_link *link, struct msgb *msg)
{
	struct msghdr mh = {};
	uint32_t seq = link->last_queued_seq + 1;
	int rc;

	aworker_ipc_frame_set_seq(msgb_data(msg), seq);
	/* MSG_NOSIGNAL is passed for completeness only: the pinned libosmocore drops these flags, see the header. */
	rc = osmo_iofd_sendmsg_msgb(link->iofd, msg, MSG_NOSIGNAL, &mh);
	if (rc < 0) {
		/* osmo_io did not take ownership */
		msgb_free(msg);
		return rc;
	}
	link->last_queued_seq = seq;
	link->tx_queued++;
	return 0;
}

static int queue_or_fail(struct aworker_ipc_link *link, struct msgb *msg)
{
	int rc = queue_record(link, msg);
	if (rc == -ENOSPC)
		link_fail(link, AWORKER_IPC_CLOSE_TX_QUEUE_FULL, rc);
	else if (rc == -ENOMEM)
		link_fail(link, AWORKER_IPC_CLOSE_NO_MEMORY, rc);
	else if (rc < 0)
		link_fail(link, AWORKER_IPC_CLOSE_TX_ERROR, rc);
	return rc;
}

static bool hello_compatible(struct aworker_ipc_link *link, const struct aworker_ipc_hello *h)
{
	if (h->role != peer_role(link)) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: role %u, expected %u\n", h->role, peer_role(link));
		return false;
	}
	if (h->worker_idx != link->worker_idx) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: worker index %u, expected %u\n", h->worker_idx,
			 link->worker_idx);
		return false;
	}
	if (h->max_frame != AWORKER_IPC_MAX_FRAME || h->max_data != AWORKER_IPC_MAX_DATA) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: limits frame=%u data=%u, expected frame=%u data=%u\n",
			 h->max_frame, h->max_data, AWORKER_IPC_MAX_FRAME, AWORKER_IPC_MAX_DATA);
		return false;
	}
	if (strcmp(h->ident, link->ident))
		LOG_LINK(link, LOGL_NOTICE, "peer HELLO: ident '%s' differs from own ident '%s'\n", h->ident,
			 link->ident);
	return true;
}

/* Takes ownership of msg. */
static void handle_rx(struct aworker_ipc_link *link, int res, struct msgb *msg, const struct msghdr *msgh)
{
	struct aworker_ipc_msg m;
	struct msgb *rsp;
	int rc;

	if (link->state == AWORKER_IPC_LINK_CLOSED) {
		msgb_free(msg);
		return;
	}
	if (res < 0) {
		msgb_free(msg);
		if (res == -EAGAIN || res == -EWOULDBLOCK || res == -EINTR)
			return;
		link_fail(link, AWORKER_IPC_CLOSE_RX_ERROR, res);
		return;
	}
	if (res == 0) {
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_PEER_EOF, 0);
		return;
	}
	if ((msgh && (msgh->msg_flags & MSG_TRUNC)) || msgb_length(msg) > AWORKER_IPC_MAX_FRAME) {
		rc = msgb_length(msg);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_RX_OVERSIZE, rc);
		return;
	}

	rc = aworker_ipc_dec(&m, msgb_data(msg), msgb_length(msg), link->role);
	if (rc < 0) {
		LOG_LINK(link, LOGL_ERROR, "received malformed record (%u octets): %s\n", msgb_length(msg),
			 aworker_ipc_err_name(rc));
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_PROTOCOL, rc);
		return;
	}
	link->rx_records++;
	if (m.seq != link->rx_next_seq) {
		LOG_LINK(link, LOGL_ERROR, "received %s with seq %u, expected %u\n", aworker_ipc_msg_type_name(m.type),
			 m.seq, link->rx_next_seq);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_BAD_SEQ, (int)m.seq);
		return;
	}
	link->rx_next_seq++;

	if (link->state == AWORKER_IPC_LINK_HELLO_WAIT) {
		msgb_free(msg);
		if (m.type != AWORKER_IPC_MT_HELLO) {
			LOG_LINK(link, LOGL_ERROR, "received %s before HELLO\n", aworker_ipc_msg_type_name(m.type));
			link_fail(link, AWORKER_IPC_CLOSE_NO_HELLO, m.type);
			return;
		}
		if (!hello_compatible(link, &m.u.hello)) {
			link_fail(link, AWORKER_IPC_CLOSE_INCOMPATIBLE, 0);
			return;
		}
		link->state = AWORKER_IPC_LINK_READY;
		LOG_LINK(link, LOGL_INFO, "peer HELLO accepted (pid %u, ident '%s')\n", m.u.hello.pid,
			 m.u.hello.ident);
		if (link->ops->ready_cb)
			link->ops->ready_cb(link, &m.u.hello);
		return;
	}

	switch (m.type) {
	case AWORKER_IPC_MT_HELLO:
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_DUP_HELLO, 0);
		return;
	case AWORKER_IPC_MT_ECHO_REQ:
		msgb_free(msg);
		rc = aworker_ipc_enc_echo(link, &rsp, true, m.u.echo_token);
		if (rc < 0) {
			link_fail(link, AWORKER_IPC_CLOSE_NO_MEMORY, rc);
			return;
		}
		queue_or_fail(link, rsp);
		return;
	default:
		link->ops->rx_cb(link, &m);
		msgb_free(msg);
		return;
	}
}

static void link_recvmsg_cb(struct osmo_io_fd *iofd, int res, struct msgb *msg, const struct msghdr *msgh)
{
	struct aworker_ipc_link *link = osmo_iofd_get_data(iofd);

	link_enter(link);
	handle_rx(link, res, msg, msgh);
	link_leave(link);
}

/* osmo_io keeps ownership of msg and frees it after this call-back. */
static void handle_tx_done(struct aworker_ipc_link *link, int res, struct msgb *msg)
{
	uint32_t seq;

	if (link->state == AWORKER_IPC_LINK_CLOSED)
		return;
	if (res < 0) {
		link_fail(link, AWORKER_IPC_CLOSE_TX_ERROR, res);
		return;
	}
	if (res != (int)msgb_length(msg)) {
		/* Not possible for an AF_UNIX SOCK_SEQPACKET socket, but osmo_io would re-queue the remainder of the
		 * record after this call-back returns. Closing the osmo_io_fd here guarantees it is never sent. */
		link_fail(link, AWORKER_IPC_CLOSE_TX_PARTIAL, res);
		return;
	}
	seq = aworker_ipc_frame_seq(msgb_data(msg));
	if (seq != link->last_written_seq + 1) {
		link_fail(link, AWORKER_IPC_CLOSE_TX_SEQ, (int)seq);
		return;
	}
	link->last_written_seq = seq;
	link->tx_written++;
}

static void link_sendmsg_cb(struct osmo_io_fd *iofd, int res, struct msgb *msg)
{
	struct aworker_ipc_link *link = osmo_iofd_get_data(iofd);

	/* osmo_io signals "writable, nothing queued" with msg == NULL */
	if (!msg)
		return;
	link_enter(link);
	handle_tx_done(link, res, msg);
	link_leave(link);
}

static const struct osmo_io_ops link_ioops = {
	.recvmsg_cb = link_recvmsg_cb,
	.sendmsg_cb = link_sendmsg_cb,
};

static int check_fd(int fd)
{
	struct sockaddr_storage ss;
	socklen_t len;
	int type;

	len = sizeof(type);
	if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) < 0)
		return -errno;
	if (type != SOCK_SEQPACKET)
		return -EPROTOTYPE;
	len = sizeof(ss);
	memset(&ss, 0, sizeof(ss));
	if (getsockname(fd, (struct sockaddr *)&ss, &len) < 0)
		return -errno;
	if (ss.ss_family != AF_UNIX)
		return -EAFNOSUPPORT;
	return 0;
}

static int set_fd_flags(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -errno;
	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return -errno;
	return 0;
}

/*! Create a link on a connected AF_UNIX SOCK_SEQPACKET socket and queue the own HELLO.
 * \param[out] out  the new link.
 * \param[in] ctx  talloc context.
 * \param[in] fd  the socket. On success the link owns it (it is set to non-blocking and close-on-exec); on failure
 *		  it is not closed.
 * \param[in] cfg  configuration (copied).
 * \param[in] ops  call-backs; rx_cb and closed_cb are mandatory. Must stay valid for the life of the link.
 * \param[in] priv  user data, see aworker_ipc_link_get_priv().
 * \returns 0; -EINVAL for an invalid cfg/ops; -ENOTSOCK, -EPROTOTYPE or -EAFNOSUPPORT if fd is not an AF_UNIX
 *	    SOCK_SEQPACKET socket; -EPIPE if SIGPIPE is not ignored; -ENOMEM; or another negative errno. */
int aworker_ipc_link_alloc(struct aworker_ipc_link **out, void *ctx, int fd, const struct aworker_ipc_link_cfg *cfg,
			   const struct aworker_ipc_link_ops *ops, void *priv)
{
	struct aworker_ipc_link *link;
	struct sigaction sa;
	struct msgb *hello;
	const char *ident;
	int rc;

	*out = NULL;
	if (!cfg || !ops || !ops->rx_cb || !ops->closed_cb)
		return -EINVAL;
	if (cfg->role != AWORKER_IPC_ROLE_MAIN && cfg->role != AWORKER_IPC_ROLE_WORKER)
		return -EINVAL;
	ident = cfg->ident ? cfg->ident : "";
	if (strlen(ident) > AWORKER_IPC_MAX_IDENT)
		return -EINVAL;

	rc = check_fd(fd);
	if (rc < 0)
		return rc;
	if (sigaction(SIGPIPE, NULL, &sa) < 0 || sa.sa_handler != SIG_IGN)
		return -EPIPE;

	link = talloc_zero(ctx, struct aworker_ipc_link);
	if (!link)
		return -ENOMEM;
	link->role = cfg->role;
	link->worker_idx = cfg->worker_idx;
	link->log_cat = cfg->log_cat;
	OSMO_STRLCPY_ARRAY(link->ident, ident);
	link->ops = ops;
	link->priv = priv;
	link->state = AWORKER_IPC_LINK_HELLO_WAIT;
	link->rx_next_seq = 1;

	/* Encode the HELLO before touching the fd, so that most failures leave the fd unmodified. */
	rc = aworker_ipc_enc_hello(link, &hello, link->role, link->worker_idx, cfg->pid, link->ident);
	if (rc < 0) {
		talloc_free(link);
		return rc;
	}

	link->iofd = osmo_iofd_setup(link, fd, link->role == AWORKER_IPC_ROLE_MAIN ? "aworker-ipc-main"
							: "aworker-ipc-worker",
				     OSMO_IO_FD_MODE_RECVMSG_SENDMSG, &link_ioops, link);
	if (!link->iofd) {
		msgb_free(hello);
		talloc_free(link);
		return -ENOMEM;
	}
	/* One octet more than the largest valid record: a larger record is detected by its length even if the
	 * backend does not report MSG_TRUNC. No headroom is needed. */
	osmo_iofd_set_alloc_info(link->iofd, AWORKER_IPC_MAX_FRAME + 1, 0);
	osmo_iofd_set_txqueue_max_length(link->iofd, cfg->txqueue_max ? cfg->txqueue_max
							      : AWORKER_IPC_DEFAULT_TXQUEUE_MAX);

	/* Queue the HELLO; it is sent once the fd is registered. Until then the osmo_io_fd is in closed state,
	 * so freeing it on failure does not close the caller's fd. */
	rc = queue_record(link, hello);
	if (rc < 0)
		goto err_free;

	rc = set_fd_flags(fd);
	if (rc < 0)
		goto err_free;

	rc = osmo_iofd_register(link->iofd, fd);
	if (rc < 0)
		goto err_free;

	*out = link;
	return 0;

err_free:
	osmo_iofd_free(link->iofd);
	talloc_free(link);
	return rc;
}

/*! Send one record built by an aworker_ipc_enc_*() function (other than HELLO). Always takes ownership of msg.
 * \param[out] seq  if not NULL, the sequence number assigned to the record.
 * \returns 0 if queued; -EBUSY before the handshake completed; -ENOTCONN if the link is closed; -EINVAL if msg is not
 *	    a valid record for this direction (nothing sent, link stays up); -ENOSPC / -ENOMEM if it could not be
 *	    queued, in which case the link is closed (closed_cb is called before this returns). */
int aworker_ipc_link_send(struct aworker_ipc_link *link, struct msgb *msg, uint32_t *seq)
{
	struct aworker_ipc_msg check;
	int rc;

	if (!msg)
		return -EINVAL;
	if (link->state == AWORKER_IPC_LINK_CLOSED) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if (link->state != AWORKER_IPC_LINK_READY) {
		msgb_free(msg);
		return -EBUSY;
	}

	/* Never emit a record the peer would reject: validate it exactly as the peer will. */
	rc = aworker_ipc_dec(&check, msgb_data(msg), msgb_length(msg), peer_role(link));
	if (rc < 0 || check.type == AWORKER_IPC_MT_HELLO) {
		LOG_LINK(link, LOGL_ERROR, "refusing to send invalid record: %s\n",
			 rc < 0 ? aworker_ipc_err_name(rc) : "HELLO is sent by the link");
		msgb_free(msg);
		return -EINVAL;
	}

	rc = queue_or_fail(link, msg);
	if (rc < 0)
		return rc;	/* the link may have been freed by closed_cb */
	if (seq)
		*seq = link->last_queued_seq;
	return 0;
}

/*! Close the link (closed_cb is called with AWORKER_IPC_CLOSE_LOCAL unless already closed). */
void aworker_ipc_link_close(struct aworker_ipc_link *link)
{
	link_fail(link, AWORKER_IPC_CLOSE_LOCAL, 0);
}

/*! Free the link. Closes it first if necessary, without calling closed_cb. Safe to call from the link's
 * call-backs; the memory is then released when the outermost call-back returns. */
void aworker_ipc_link_free(struct aworker_ipc_link *link)
{
	if (!link)
		return;
	if (link->state != AWORKER_IPC_LINK_CLOSED) {
		link->state = AWORKER_IPC_LINK_CLOSED;
		osmo_iofd_close(link->iofd);
	}
	if (link->cb_depth) {
		link->free_pending = true;
		return;
	}
	link_destroy(link);
}

void *aworker_ipc_link_get_priv(const struct aworker_ipc_link *link)
{
	return link->priv;
}

enum aworker_ipc_link_state aworker_ipc_link_get_state(const struct aworker_ipc_link *link)
{
	return link->state;
}

void aworker_ipc_link_get_stats(const struct aworker_ipc_link *link, struct aworker_ipc_link_stats *stats)
{
	*stats = (struct aworker_ipc_link_stats){
		.state = link->state,
		.rx_records = link->rx_records,
		.tx_queued = link->tx_queued,
		.tx_written = link->tx_written,
		.last_written_seq = link->last_written_seq,
		.last_queued_seq = link->last_queued_seq,
	};
}

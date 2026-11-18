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
#include <unistd.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/osmo_io.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>

#define AWORKER_IPC_DEFAULT_TXQUEUE_MAX	1024
/* Received descriptors kept per record before they are checked (a correct record has at most one; the rest of a
 * wrong one is closed as it is found). */
#define RX_FD_KEEP	4

/* Sender: the descriptor of a queued record, kept open until that record was sent (or discarded). */
struct pending_fd {
	struct llist_head list;
	uint32_t seq;
	int fd;
};

struct aworker_ipc_link {
	struct osmo_io_fd *iofd;
	enum aworker_ipc_role role;
	uint16_t worker_idx;
	int log_cat;
	char ident[AWORKER_IPC_MAX_IDENT + 1];
	struct aworker_ipc_identity id;
	unsigned int classes;		/* allowed classes, LINK included */
	bool fd_passing;
	struct llist_head pending_fds;	/* struct pending_fd, in seq order */
	int rx_fd;			/* the current record's descriptor during rx_cb, -1 */
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
	{ AWORKER_IPC_CLOSE_CLASS, "class" },
	{ AWORKER_IPC_CLOSE_RX_FD, "rx-fd" },
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

/* The transmit queue was discarded (or the link is going away): close the descriptors of its records. */
static void close_pending_fds(struct aworker_ipc_link *link)
{
	struct pending_fd *p, *p2;

	llist_for_each_entry_safe(p, p2, &link->pending_fds, list) {
		close(p->fd);
		llist_del(&p->list);
		talloc_free(p);
	}
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
	close_pending_fds(link);

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

/* Assign the next sequence number and hand the record -- with fd as SCM_RIGHTS if fd >= 0 -- to osmo_io. Always
 * takes ownership of msg; takes ownership of fd only on success (then it is closed once the record was sent or
 * discarded, see close_pending_fds()). */
static int queue_record(struct aworker_ipc_link *link, struct msgb *msg, int fd)
{
	union {
		char buf[CMSG_SPACE(sizeof(int))];
		struct cmsghdr align;
	} cbuf;
	struct msghdr mh = {};
	struct pending_fd *p = NULL;
	uint32_t seq = link->last_queued_seq + 1;
	int rc;

	if (fd >= 0) {
		struct cmsghdr *cm;
		p = talloc_zero(link, struct pending_fd);
		if (!p) {
			msgb_free(msg);
			return -ENOMEM;
		}
		memset(&cbuf, 0, sizeof(cbuf));
		mh.msg_control = cbuf.buf;
		mh.msg_controllen = sizeof(cbuf.buf);
		cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	}
	aworker_ipc_frame_set_seq(msgb_data(msg), seq);
	/* MSG_NOSIGNAL is passed for completeness only: the pinned libosmocore drops these flags, see the header.
	 * osmo_io copies the control buffer (the descriptor's number) now; the kernel takes the descriptor only when
	 * the record is actually sent, so fd stays open until then. */
	rc = osmo_iofd_sendmsg_msgb(link->iofd, msg, MSG_NOSIGNAL, &mh);
	if (rc < 0) {
		/* osmo_io did not take ownership */
		msgb_free(msg);
		talloc_free(p);
		return rc;
	}
	if (p) {
		p->seq = seq;
		p->fd = fd;
		llist_add_tail(&p->list, &link->pending_fds);
	}
	link->last_queued_seq = seq;
	link->tx_queued++;
	return 0;
}

/* As queue_record(), closing the link on failure; takes ownership of fd in every case. */
static int queue_or_fail(struct aworker_ipc_link *link, struct msgb *msg, int fd)
{
	int rc = queue_record(link, msg, fd);
	if (rc < 0 && fd >= 0)
		close(fd);
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
	/* Identity: what both sides assert must agree. */
	if (h->id.boot_nonce && link->id.boot_nonce && h->id.boot_nonce != link->id.boot_nonce) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: boot nonce %016llx, expected %016llx\n",
			 (unsigned long long)h->id.boot_nonce, (unsigned long long)link->id.boot_nonce);
		return false;
	}
	if (h->id.incarnation && link->id.incarnation && h->id.incarnation != link->id.incarnation) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: incarnation %u, expected %u\n", h->id.incarnation,
			 link->id.incarnation);
		return false;
	}
	if (h->id.session_id && link->id.session_id && h->id.session_id != link->id.session_id) {
		LOG_LINK(link, LOGL_ERROR, "peer HELLO: session %u, expected %u\n", h->id.session_id,
			 link->id.session_id);
		return false;
	}
	if (strcmp(h->ident, link->ident))
		LOG_LINK(link, LOGL_NOTICE, "peer HELLO: ident '%s' differs from own ident '%s'\n", h->ident,
			 link->ident);
	return true;
}

/* The descriptors that came with a record: made close-on-exec at once; the first RX_FD_KEEP kept, any others closed
 * right away (the record is wrong then anyway). *fd_err: -EMSGSIZE for MSG_CTRUNC, -EPROTO for a control message that
 * is not SCM_RIGHTS. \returns how many descriptors came. */
static unsigned int rx_fds_collect(const struct msghdr *msgh, int *fds, int *fd_err)
{
	struct cmsghdr *cm;
	unsigned int total = 0;

	*fd_err = 0;
	if (!msgh)
		return 0;
	if (msgh->msg_flags & MSG_CTRUNC)
		*fd_err = -EMSGSIZE;
	for (cm = CMSG_FIRSTHDR((struct msghdr *)msgh); cm; cm = CMSG_NXTHDR((struct msghdr *)msgh, cm)) {
		size_t i, n;
		if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
			*fd_err = -EPROTO;
			continue;
		}
		n = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (i = 0; i < n; i++) {
			int fd, flags;
			memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
			flags = fcntl(fd, F_GETFD);
			if (flags >= 0)
				fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
			if (total < RX_FD_KEEP)
				fds[total] = fd;
			else
				close(fd);
			total++;
		}
	}
	return total;
}

static void rx_fds_close(int *fds, unsigned int *total)
{
	unsigned int i;

	for (i = 0; i < *total && i < RX_FD_KEEP; i++)
		close(fds[i]);
	*total = 0;
}

/* Takes ownership of msg, and of every descriptor that came with it (msgh). */
static void handle_rx(struct aworker_ipc_link *link, int res, struct msgb *msg, const struct msghdr *msgh)
{
	struct aworker_ipc_msg m;
	struct msgb *rsp;
	int fds[RX_FD_KEEP];
	unsigned int nfds;
	int rc, fd_err;

	/* Every descriptor that came is owned here from now on: each path below hands it on or closes it. */
	nfds = res > 0 ? rx_fds_collect(msgh, fds, &fd_err) : 0;
	if (res <= 0)
		fd_err = 0;

	if (link->state == AWORKER_IPC_LINK_CLOSED) {
		rx_fds_close(fds, &nfds);
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
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_RX_OVERSIZE, rc);
		return;
	}
	if (fd_err) {
		LOG_LINK(link, LOGL_ERROR, "received a record with %s\n",
			 fd_err == -EMSGSIZE ? "more descriptors than one (MSG_CTRUNC)" : "a control message other than"
			 " SCM_RIGHTS");
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_RX_FD, fd_err);
		return;
	}

	rc = aworker_ipc_dec(&m, msgb_data(msg), msgb_length(msg), link->role);
	if (rc < 0) {
		LOG_LINK(link, LOGL_ERROR, "received malformed record (%u octets): %s\n", msgb_length(msg),
			 aworker_ipc_err_name(rc));
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_PROTOCOL, rc);
		return;
	}
	link->rx_records++;
	if (!(aworker_ipc_msg_class(m.type) & link->classes)) {
		LOG_LINK(link, LOGL_ERROR, "received %s, of class %s, which this link does not carry\n",
			 aworker_ipc_msg_type_name(m.type), aworker_ipc_class_name(aworker_ipc_msg_class(m.type)));
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_CLASS, m.type);
		return;
	}
	if (m.seq != link->rx_next_seq) {
		LOG_LINK(link, LOGL_ERROR, "received %s with seq %u, expected %u\n", aworker_ipc_msg_type_name(m.type),
			 m.seq, link->rx_next_seq);
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_BAD_SEQ, (int)m.seq);
		return;
	}
	link->rx_next_seq++;
	if (nfds != aworker_ipc_msg_fds(m.type)) {
		LOG_LINK(link, LOGL_ERROR, "received %s with %u descriptor(s), it takes %u\n",
			 aworker_ipc_msg_type_name(m.type), nfds, aworker_ipc_msg_fds(m.type));
		rc = (int)nfds;
		rx_fds_close(fds, &nfds);
		msgb_free(msg);
		link_fail(link, AWORKER_IPC_CLOSE_RX_FD, rc);
		return;
	}
	/* From here on, a descriptor (nfds == 1) can only belong to a record that takes one: neither HELLO nor
	 * ECHO_REQ does. */

	if (link->state == AWORKER_IPC_LINK_HELLO_WAIT) {
		msgb_free(msg);
		if (m.type != AWORKER_IPC_MT_HELLO) {
			LOG_LINK(link, LOGL_ERROR, "received %s before HELLO\n", aworker_ipc_msg_type_name(m.type));
			rx_fds_close(fds, &nfds);
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
		queue_or_fail(link, rsp, -1);
		return;
	default:
		/* The descriptor, if any, is the link's until rx_cb takes it (aworker_ipc_link_take_fd()). */
		m.fd = nfds ? fds[0] : -1;
		link->rx_fd = m.fd;
		link->ops->rx_cb(link, &m);
		if (link->rx_fd >= 0)
			close(link->rx_fd);
		link->rx_fd = -1;
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
	/* Sent: the kernel holds its own reference to the record's descriptor now; close ours. */
	if (!llist_empty(&link->pending_fds)) {
		struct pending_fd *p = llist_first_entry(&link->pending_fds, struct pending_fd, list);
		if (p->seq == seq) {
			close(p->fd);
			llist_del(&p->list);
			talloc_free(p);
		}
	}
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
	link->id = cfg->id;
	link->fd_passing = cfg->fd_passing;
	INIT_LLIST_HEAD(&link->pending_fds);
	link->rx_fd = -1;
	link->classes = AWORKER_IPC_CLASS_LINK
			| (cfg->classes ? cfg->classes : AWORKER_IPC_CLASS_CONTROL | AWORKER_IPC_CLASS_DATA);
	link->ops = ops;
	link->priv = priv;
	link->state = AWORKER_IPC_LINK_HELLO_WAIT;
	link->rx_next_seq = 1;

	/* Encode the HELLO before touching the fd, so that most failures leave the fd unmodified. */
	rc = aworker_ipc_enc_hello_id(link, &hello, link->role, link->worker_idx, cfg->pid, link->ident, &link->id);
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
	/* Receiving descriptors: room for exactly one per record. Without it, one sent to us is MSG_CTRUNC. */
	if (link->fd_passing && osmo_iofd_set_cmsg_size(link->iofd, CMSG_SPACE(sizeof(int))) < 0) {
		msgb_free(hello);
		osmo_iofd_free(link->iofd);
		talloc_free(link);
		return -ENOMEM;
	}

	/* Queue the HELLO; it is sent once the fd is registered. Until then the osmo_io_fd is in closed state,
	 * so freeing it on failure does not close the caller's fd. */
	rc = queue_record(link, hello, -1);
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
/* aworker_ipc_link_send() and _send_fd(): takes ownership of msg and fd (-1: none) in every case. */
static int link_send(struct aworker_ipc_link *link, struct msgb *msg, int fd, uint32_t *seq)
{
	struct aworker_ipc_msg check;
	int rc;

	if (!msg) {
		if (fd >= 0)
			close(fd);
		return -EINVAL;
	}
	if (link->state == AWORKER_IPC_LINK_CLOSED) {
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -ENOTCONN;
	}
	if (link->state != AWORKER_IPC_LINK_READY) {
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -EBUSY;
	}

	/* Never emit a record the peer would reject: validate it exactly as the peer will. */
	rc = aworker_ipc_dec(&check, msgb_data(msg), msgb_length(msg), peer_role(link));
	if (rc < 0 || check.type == AWORKER_IPC_MT_HELLO) {
		LOG_LINK(link, LOGL_ERROR, "refusing to send invalid record: %s\n",
			 rc < 0 ? aworker_ipc_err_name(rc) : "HELLO is sent by the link");
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -EINVAL;
	}
	if (!(aworker_ipc_msg_class(check.type) & link->classes)) {
		LOG_LINK(link, LOGL_ERROR, "refusing to send %s: class %s, which this link does not carry\n",
			 aworker_ipc_msg_type_name(check.type), aworker_ipc_class_name(aworker_ipc_msg_class(check.type)));
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -EINVAL;
	}
	if ((fd >= 0 ? 1u : 0u) != aworker_ipc_msg_fds(check.type)) {
		LOG_LINK(link, LOGL_ERROR, "refusing to send %s %s a descriptor: it takes %u\n",
			 aworker_ipc_msg_type_name(check.type), fd >= 0 ? "with" : "without",
			 aworker_ipc_msg_fds(check.type));
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -EINVAL;
	}

	rc = queue_or_fail(link, msg, fd);
	if (rc < 0)
		return rc;	/* the link may have been freed by closed_cb */
	if (seq)
		*seq = link->last_queued_seq;
	return 0;
}

int aworker_ipc_link_send(struct aworker_ipc_link *link, struct msgb *msg, uint32_t *seq)
{
	return link_send(link, msg, -1, seq);
}

int aworker_ipc_link_send_fd(struct aworker_ipc_link *link, struct msgb *msg, int fd, uint32_t *seq)
{
	if (fd < 0 || fcntl(fd, F_GETFD) < 0) {
		msgb_free(msg);
		return -EBADF;
	}
	return link_send(link, msg, fd, seq);
}

int aworker_ipc_link_take_fd(struct aworker_ipc_link *link)
{
	int fd = link->rx_fd;
	link->rx_fd = -1;
	return fd;
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
	/* Records still queued are discarded with the link: so are their descriptors. */
	close_pending_fds(link);
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

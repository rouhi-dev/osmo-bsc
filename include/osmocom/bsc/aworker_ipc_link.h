/* A-worker IPC: one AF_UNIX SOCK_SEQPACKET link between the osmo-bsc main process and one worker */
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
#pragma once

/* LINK BEHAVIOUR
 * ==============
 *
 * The link runs on an already connected AF_UNIX SOCK_SEQPACKET socket (normally one end of a socketpair()) through
 * osmo_io in OSMO_IO_FD_MODE_RECVMSG_SENDMSG mode: exactly one record per sendmsg()/recvmsg(). The link never
 * changes the osmo_io buffer counts (more than one buffer would merge or split records).
 *
 * Lifecycle: HELLO_WAIT -> READY -> CLOSED. Each side queues its HELLO (seq 1) on allocation. The first record
 * received must be a HELLO with seq 1, the opposite role, the same worker index and the same frame/data limits, and
 * no identity that contradicts the own one (cfg->id): a boot nonce, incarnation or session_id that both sides
 * assert (non-zero) must be equal. Otherwise the link closes (AWORKER_IPC_CLOSE_INCOMPATIBLE). The peer's minor
 * version may differ (see aworker_ipc.h). aworker_ipc_link_send() returns -EBUSY until the peer's HELLO was accepted (then
 * ready_cb is called) and -ENOTCONN once closed. Received sequence numbers must increase by exactly 1.
 * ECHO_REQ is answered by the link itself; all other messages after HELLO are passed to rx_cb.
 *
 * Classes (aworker_ipc.h): a link carries the LINK class and the classes of cfg->classes (0: CONTROL and DATA, the
 * worker link as it always was). A received record of another class closes the link (AWORKER_IPC_CLOSE_CLASS);
 * aworker_ipc_link_send() refuses to send one (-EINVAL; nothing is sent, the link stays up).
 *
 * Fail closed: every receive error, peer EOF, oversized/truncated record (MSG_TRUNC or > AWORKER_IPC_MAX_FRAME),
 * decode error, sequence error, send error, partial send, full transmit queue or allocation failure closes the link:
 * the socket is closed and the transmit queue discarded, so a partially sent record can never be followed by its
 * remainder. Nothing is retried, except what osmo_io does for EAGAIN: it re-queues the complete, unsent record.
 * closed_cb is called exactly once, unless the link is freed before it is closed.
 *
 * SIGPIPE: osmo_iofd_sendmsg_msgb() of the pinned libosmocore does not pass its flags argument to sendmsg()
 * (MSG_NOSIGNAL is lost), so writing to a closed peer raises SIGPIPE. aworker_ipc_link_alloc() therefore refuses
 * to run unless SIGPIPE is ignored (osmo_init_ignore_signals()).
 *
 * Delivery accounting: aworker_ipc_link_send() returns the record's sequence number; the closed_cb info reports
 * the highest sequence number that the kernel fully accepted (last_written_seq). Records with a higher sequence
 * number never left this process.
 *
 * DESCRIPTORS (since 2.0; SCM_RIGHTS on this link; at most one per record, aworker_ipc_msg_fds() of its type):
 * Sending (aworker_ipc_link_send_fd()): the link takes the descriptor over in every case. osmo_io copies only its
 * number when the record is queued, and the kernel duplicates it only when the record is actually sent, so the link
 * keeps it open until the record's send completed, then closes it; if the record is discarded instead -- the link
 * closes or is freed with it still queued, or it could not be queued -- the link closes it then. A record of a type
 * that takes a descriptor cannot be sent without one (aworker_ipc_link_send() refuses it), and vice versa.
 * Receiving (only with cfg->fd_passing; a link without it has no control buffer, so a descriptor sent to it makes
 * the kernel report MSG_CTRUNC): every received descriptor is made close-on-exec at once (osmo_io cannot pass
 * MSG_CMSG_CLOEXEC). MSG_CTRUNC, a control message other than SCM_RIGHTS, or a descriptor count that is not exactly
 * that of the record's type closes every received descriptor, then the link (AWORKER_IPC_CLOSE_RX_FD); so does every
 * other receive error of a record that came with descriptors. A record's descriptor is handed to rx_cb in msg->fd;
 * rx_cb takes it with aworker_ipc_link_take_fd(), otherwise the link closes it when rx_cb returns. Every received
 * descriptor thus has exactly one owner, or is closed.
 */

#include <stdint.h>

#include <osmocom/bsc/aworker_ipc.h>

struct msgb;
struct aworker_ipc_link;

enum aworker_ipc_link_state {
	AWORKER_IPC_LINK_HELLO_WAIT,
	AWORKER_IPC_LINK_READY,
	AWORKER_IPC_LINK_CLOSED,
};

enum aworker_ipc_close_reason {
	AWORKER_IPC_CLOSE_LOCAL = 1,		/*!< aworker_ipc_link_close() */
	AWORKER_IPC_CLOSE_PEER_EOF,		/*!< peer closed its end */
	AWORKER_IPC_CLOSE_RX_ERROR,		/*!< recvmsg() error; err = -errno */
	AWORKER_IPC_CLOSE_RX_OVERSIZE,		/*!< record > AWORKER_IPC_MAX_FRAME or MSG_TRUNC */
	AWORKER_IPC_CLOSE_PROTOCOL,		/*!< decode error; err = -enum aworker_ipc_err */
	AWORKER_IPC_CLOSE_BAD_SEQ,		/*!< received sequence number not previous + 1 */
	AWORKER_IPC_CLOSE_NO_HELLO,		/*!< first record was not a HELLO */
	AWORKER_IPC_CLOSE_DUP_HELLO,		/*!< HELLO after the handshake */
	AWORKER_IPC_CLOSE_INCOMPATIBLE,		/*!< peer HELLO: wrong role, worker index or limits, or a
						 *   contradicting identity */
	AWORKER_IPC_CLOSE_TX_ERROR,		/*!< sendmsg() error; err = -errno */
	AWORKER_IPC_CLOSE_TX_PARTIAL,		/*!< sendmsg() accepted only part of a record; err = bytes sent */
	AWORKER_IPC_CLOSE_TX_SEQ,		/*!< send completions out of order */
	AWORKER_IPC_CLOSE_TX_QUEUE_FULL,	/*!< transmit queue full */
	AWORKER_IPC_CLOSE_NO_MEMORY,		/*!< allocation failure */
	AWORKER_IPC_CLOSE_CLASS,		/*!< a record of a class this link does not carry; err = msg type */
	AWORKER_IPC_CLOSE_RX_FD,		/*!< descriptors received wrongly: err = -EMSGSIZE (MSG_CTRUNC),
						 *   -EPROTO (not SCM_RIGHTS), or the number received */
};

const char *aworker_ipc_close_reason_name(enum aworker_ipc_close_reason reason);

struct aworker_ipc_link_close_info {
	enum aworker_ipc_close_reason reason;
	int err;
	uint32_t last_written_seq;	/*!< highest seq fully accepted by the kernel (0: none) */
	uint32_t last_queued_seq;	/*!< highest seq handed to osmo_io */
};

struct aworker_ipc_link_ops {
	/*! The peer's HELLO was accepted; sending is possible from now on. */
	void (*ready_cb)(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer);
	/*! A message other than HELLO / ECHO_REQ. DATA views in msg are only valid during the call. */
	void (*rx_cb)(struct aworker_ipc_link *link, const struct aworker_ipc_msg *msg);
	/*! The link was closed (exactly once). */
	void (*closed_cb)(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info);
};

struct aworker_ipc_link_cfg {
	enum aworker_ipc_role role;	/*!< own role */
	uint16_t worker_idx;		/*!< worker index; must match the peer's */
	uint32_t pid;			/*!< sent in HELLO, informational */
	const char *ident;		/*!< sent in HELLO, informational (<= 64 printable ASCII characters) */
	struct aworker_ipc_identity id;	/*!< sent in HELLO; checked against the peer's (0 fields: not asserted) */
	unsigned int classes;		/*!< enum aworker_ipc_class set besides LINK; 0: CONTROL | DATA */
	bool fd_passing;		/*!< receive descriptors (a control buffer for one per record) */
	unsigned int txqueue_max;	/*!< 0: default (1024 records) */
	int log_cat;			/*!< logging category */
};

struct aworker_ipc_link_stats {
	enum aworker_ipc_link_state state;
	uint32_t rx_records;
	uint32_t tx_queued;
	uint32_t tx_written;
	uint32_t last_written_seq;
	uint32_t last_queued_seq;
};

int aworker_ipc_link_alloc(struct aworker_ipc_link **out, void *ctx, int fd, const struct aworker_ipc_link_cfg *cfg,
			   const struct aworker_ipc_link_ops *ops, void *priv);
int aworker_ipc_link_send(struct aworker_ipc_link *link, struct msgb *msg, uint32_t *seq);
/*! Send a record of a type that carries a descriptor (ATTACH), with \a fd. Takes ownership of msg AND fd in every
 * case (fd is closed on every failure). \returns 0, or as aworker_ipc_link_send(); -EBADF if fd is not open. */
int aworker_ipc_link_send_fd(struct aworker_ipc_link *link, struct msgb *msg, int fd, uint32_t *seq);
/*! In rx_cb only: take the descriptor received with the current record (msg->fd). \returns it, or -1 if there is none
 * or it was taken already. The caller owns it from then on. */
int aworker_ipc_link_take_fd(struct aworker_ipc_link *link);
void aworker_ipc_link_close(struct aworker_ipc_link *link);
void aworker_ipc_link_free(struct aworker_ipc_link *link);

void *aworker_ipc_link_get_priv(const struct aworker_ipc_link *link);
enum aworker_ipc_link_state aworker_ipc_link_get_state(const struct aworker_ipc_link *link);
void aworker_ipc_link_get_stats(const struct aworker_ipc_link *link, struct aworker_ipc_link_stats *stats);

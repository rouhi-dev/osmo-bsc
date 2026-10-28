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
 * received must be a HELLO with seq 1, the opposite role, the same worker index and the same frame/data limits;
 * otherwise the link closes. aworker_ipc_link_send() returns -EBUSY until the peer's HELLO was accepted (then
 * ready_cb is called) and -ENOTCONN once closed. Received sequence numbers must increase by exactly 1.
 * ECHO_REQ is answered by the link itself; all other messages after HELLO are passed to rx_cb.
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
	AWORKER_IPC_CLOSE_INCOMPATIBLE,		/*!< peer HELLO: wrong role, worker index or limits */
	AWORKER_IPC_CLOSE_TX_ERROR,		/*!< sendmsg() error; err = -errno */
	AWORKER_IPC_CLOSE_TX_PARTIAL,		/*!< sendmsg() accepted only part of a record; err = bytes sent */
	AWORKER_IPC_CLOSE_TX_SEQ,		/*!< send completions out of order */
	AWORKER_IPC_CLOSE_TX_QUEUE_FULL,	/*!< transmit queue full */
	AWORKER_IPC_CLOSE_NO_MEMORY,		/*!< allocation failure */
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
void aworker_ipc_link_close(struct aworker_ipc_link *link);
void aworker_ipc_link_free(struct aworker_ipc_link *link);

void *aworker_ipc_link_get_priv(const struct aworker_ipc_link *link);
enum aworker_ipc_link_state aworker_ipc_link_get_state(const struct aworker_ipc_link *link);
void aworker_ipc_link_get_stats(const struct aworker_ipc_link *link, struct aworker_ipc_link_stats *stats);

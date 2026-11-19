/* A-worker Supervisor: the session socket, BSC sessions and the operator channel (S2-5) */
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

/* THE SOCKET (N5, decided for S2-5)
 * =================================
 * Only with 'supervisor-socket PATH' (none: no socket at all, bsc-aworker as in S2-3). Claimed before any worker
 * starts; refusing it ends bsc-aworker:
 *  - PATH is absolute (the VTY checks it): no abstract-namespace socket;
 *  - its directory exists (it is never created), is a directory, belongs to this process' effective user and is
 *    writable by nobody else (mode & 022 == 0). umask alone is not relied on;
 *  - PATH.lock (regular file, this user's, 0600, created if missing, never removed) is held with an exclusive
 *    non-blocking flock() for the Supervisor's lifetime. Held by another process: refused (a second Supervisor);
 *  - only then a leftover socket at PATH is removed; anything at PATH that is not a socket: refused;
 *  - bound with umask 0177 and chmod 0600; removed again at a regular stop.
 * Every accepted connection is checked with SO_PEERCRED before anything is read from it: its uid must be this
 * process' effective uid, or 0. Anything else is closed at once (logged). This holds for both roles.
 *
 * CONNECTIONS
 * ===========
 * A link (aworker_ipc_link.h) in the Supervisor role, SESSION class, receiving descriptors. The first record decides
 * what it is: SESSION_OPEN (a BSC) or OP_REQUEST (an operator); anything else, or nothing within
 * AWORKER_SESSION_FIRST_RECORD_S, closes it. At most AWORKER_SESSION_MAX_CONNS at a time.
 *
 * THE SESSION (one at a time; SUP 9.1)
 * ====================================
 * NONE/CLOSED --SESSION_OPEN admitted--> ACTIVE <--ECHO answered-- UNRESPONSIVE (first unanswered ECHO)
 * ACTIVE/UNRESPONSIVE --FENCE_SESSION--> FENCING --pidfd: exit / EOF--> CLOSING
 * ACTIVE/UNRESPONSIVE/FENCING --SESSION_CLOSE, EOF, link error--> CLOSING --every worker detached or gone--> CLOSED
 * Admission (SESSION_OPEN): refused with SESSION_REJECT (busy: a session exists, or a worker has not detached from the
 * last one; version; bad-document: not a memfd with all four seals of exactly doc_len octets; config-mismatch: the
 * canonical documents differ, the first difference named; no-sessions: stopping). Admitted: SESSION_ACCEPT(session
 * id, never reused within this boot; boot nonce; workers), the BSC's pid is taken from SO_PEERCRED and a pidfd opened
 * for fencing (none: fencing is refused for this session).
 * Every READY worker is then attached: a socketpair per worker, ATTACH(session, incarnation) with one end to the
 * worker, WORKER_UP(idx, incarnation, session, cs7) with the other to the BSC; the Supervisor keeps no copy of either
 * once sent (the link closes its own when the send completed). The worker counts as ATTACHED (WORKER_STATE to the BSC)
 * only when its STATE(attach-ok) arrived AND the WORKER_UP was written. A worker that becomes READY later (a restart)
 * is attached the same way; a worker lost gives WORKER_DOWN(lost); one that detached itself (its data link's EOF or
 * error) WORKER_DOWN(detached), and it is attached again after AWORKER_SESSION_REATTACH_MS.
 * CLOSING's entry action runs exactly once: ECHO stopped, the session's connection closed, DETACH(session) to every
 * worker attached or attaching to it, the session id retired. Every later EOF, CLOSE, fence result or ECHO is ignored.
 * Session ECHO (D-S12): every session-echo-interval s; the first unanswered one makes the session UNRESPONSIVE;
 * session-echo-misses in a row are reported as the threshold, and nothing more is done: the workers and their data
 * links are never touched, and no second BSC is admitted. Recovery: an answer, the BSC's end (EOF), or fencing.
 * Fencing (operator only): SIGKILL through the pidfd, then the pidfd must report the exit within
 * AWORKER_SESSION_FENCE_CONFIRM_MS: CLOSING; otherwise FENCE_FAILED: the session stays FENCING and nothing is admitted
 * (a further FENCE_SESSION tries again). Without a pidfd fencing is refused and the session is left as it is.
 *
 * THE OPERATOR CHANNEL (D-S6: no VTY server)
 * ==========================================
 * OP_REQUEST STATUS, RESET_WORKER idx, FENCE_SESSION, POOL_SHUTDOWN; each answered with OP_RESULT(rc, text). Nothing
 * else exists: the configuration cannot be changed through it.
 */

#include <stdbool.h>
#include <stdint.h>

#define AWORKER_SESSION_MAX_CONNS		8
#define AWORKER_SESSION_FIRST_RECORD_S		10
#define AWORKER_SESSION_REJECT_LINGER_S		5
#define AWORKER_SESSION_FENCE_CONFIRM_MS	3000
#define AWORKER_SESSION_REATTACH_MS		1000

/*! Test hooks: bsc-aworker's hidden --aworker-test-hook NAME[=VALUE]. Never set in production. */
struct aworker_session_test_hooks {
	unsigned int detach_delay_ms;	/*!< send CLOSING's DETACHes this much later (detach-delay-ms=N) */
	bool no_pidfd;			/*!< as if pidfd_open() failed (no-pidfd) */
	int fence_signal;		/*!< the signal fencing sends; -1: SIGKILL (fence-signal=N; 0: none) */
	long peer_uid;			/*!< the uid accepted besides root; -1: our effective uid (peer-uid=N) */
};

/*! Claim the socket (see THE SOCKET), with doc as this Supervisor's canonical document (copied). Before the pool
 * starts. \returns 0, or a negative errno (the reason is logged and printed on stderr). */
int aworker_session_start(void *ctx, const char *path, const char *doc, size_t doc_len,
			  const struct aworker_session_test_hooks *th);
/*! At the stop of bsc-aworker, before aworker_pool_stop(): SESSION_CLOSE(shutdown) to the BSC, the pending answers
 * written (bounded), every connection closed, the socket removed, the lock released. */
void aworker_session_stop(void);
/*! How often CLOSING was entered (for the tests: exactly once per session). */
unsigned int aworker_session_closing_entries(void);

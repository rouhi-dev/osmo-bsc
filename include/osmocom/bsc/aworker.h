/* A-worker: the worker process that owns the A-interface SIGTRAN stack (cs7 / SCTP / M3UA / SCCP) */
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

#include <stdbool.h>
#include <stdint.h>

/*! Run the A-worker in this process and never return.
 *
 * The worker is a child of the osmo-bsc main process, forked after the main configuration file was read and
 * before the main process opens any socket of its own, so it inherits only the log targets. It then reads its
 * own config file, which must contain nothing but cs7 instances (with their asp / as / route nodes) and
 * optionally log settings: reading it is what creates the ASPs and opens the SCTP associations, in this process
 * only.
 *
 * For every cs7 instance that has at least one ASP, the worker sets up an SCCP instance (through the same
 * osmo_sccp_simple_client_on_ss7_id() call the in-process mode uses) and reports the instance to the main
 * process. It then binds the SCCP users the main process asks for and forwards SCCP operations in both
 * directions over the IPC link.
 *
 * Snapshot mode (inline form, S2-2 decision D-6): instead of a config file, the worker gets its view of the main
 * configuration file as a snapshot (aworker_snapshot.h) on snapshot_fd. It reads it completely and closes the
 * descriptor (aworker_snapshot_read()), checks the view (aworker_cfg_check_worker_view(), D-13) and only then parses
 * it -- a view refused by either is never parsed, so no ASP and no socket is created. After the parse, the cs7
 * instances with transport nodes must be exactly the one the view's 'a-worker-pool' section declares for this
 * worker. That section is allowed only in snapshot mode; a legacy config file with it is refused as before.
 *
 * \param[in] fd the worker's end of the AF_UNIX SOCK_SEQPACKET link to the main process.
 * \param[in] worker_idx this worker's index in the pool.
 * \param[in] cfg_file the worker's config file (legacy), or NULL in snapshot mode.
 * \param[in] snapshot_fd the snapshot's descriptor (snapshot mode), or -1 with cfg_file.
 * \returns never; the process exits with one of the AWORKER_EXIT_* codes.
 */
void aworker_run(void *ctx, int fd, unsigned int worker_idx, const char *cfg_file, int snapshot_fd)
	__attribute__((noreturn));

/*! Split links (S2-5; bsc-aworker's workers, set by its worker role before aworker_run()). false (osmo-bsc's workers,
 * legacy): one link to the main process carries control and SCCP-user traffic, exactly as before.
 * true: fd is the CONTROL link to the Supervisor (CS7_INFO, ATTACH, DETACH, STATE, GOODBYE; its loss ends the worker).
 * SCCP-user traffic runs on a DATA link to the attached BSC, whose descriptor arrives with ATTACH:
 *  - ATTACH(session, incarnation) is accepted only while DETACHED and only for this process' own incarnation
 *    (aworker_incarnation); otherwise the descriptor is closed unused and STATE(attach-refused) answers. The data link's
 *    HELLO asserts the Supervisor's boot nonce, the incarnation and the session: a BSC end of another session or worker
 *    is refused by the handshake;
 *  - DETACH(session), or the data link's end (EOF: the BSC is gone; or an error), detaches: every SCCP user is unbound
 *    (osmo_sccp_user_unbind(); minimal detach, S2-5 -- the connection release of S2-6 is not done), the data link is
 *    closed, and STATE(detached, cause, users unbound) is sent. A DETACH for a session this worker is not attached to
 *    is answered with STATE(detached) again (idempotent);
 *  - SS7, ASP/AS and SCTP are never touched by attaching or detaching. */
extern bool aworker_split_links;

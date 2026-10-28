/* A-worker pool: the osmo-bsc main process side of the A-interface (SIGTRAN) worker processes */
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

/* OWNERSHIP MODEL
 * ===============
 *
 * With 'a-worker-pool' configured, a fixed number of worker processes own the SIGTRAN stack of the A interface:
 * each worker owns one or more complete cs7 instances with their ASPs, ASs, routes, SCTP associations, M3UA/IPA
 * encoding, SCCP instance, SCCP connection state and SCCP timers. The main process owns everything else:
 * BSSAP/BSSMAP/DTAP, the subscriber connection FSMs, paging, the MSC pool, BSSMAP RESET, Abis/RSL/lchan and the
 * MGW state, and in particular it allocates the SCCP connection ids and keeps the conn_id -> gscon registry.
 *
 * A cs7 instance is owned by exactly one worker, and a worker owns every cs7 instance for which its config file
 * defines an ASP. The worker reports its instances in CS7_INFO after the IPC handshake, and the main process
 * derives its cs7 instance -> worker routing table from those reports. This mapping is what makes the SCCP local
 * references unambiguous: all connections of one cs7 instance, in both directions, live in one worker, so the
 * single conn_id space of a cs7 instance is shared by exactly one allocator (the main process) and one SCCP
 * instance (that worker's).
 *
 * The main process must not parse a cs7 configuration that contains ASP, AS or xUA server nodes: reading those
 * would open SCTP associations in the main process. aworker_pool_check_main_cs7() enforces this. The main
 * process' own cs7 sections hold only the inert configuration it needs itself: point code, point code format and
 * the SCCP address book.
 *
 * FAILURE POLICY
 * ==============
 *
 * A TX_* request handed to a worker is never retried and never replayed, whatever happens afterwards: an SCCP
 * operation whose delivery status is uncertain (the worker reports TX_FAILED with disposition EXECUTED_ERROR, or
 * the link or the worker dies after the record was queued) tears the affected SCCP connection down locally
 * instead. When a worker is lost, every SCCP connection of its cs7 instances is terminated and the BSSMAP RESET
 * FSM of every MSC on them is put back into DISC state, so that the MSC relationship is rebuilt from scratch.
 * 'on-worker-failure restart' then restarts the worker after 'restart-delay' seconds; 'shutdown' terminates
 * osmo-bsc.
 */

#include <stdbool.h>
#include <stdint.h>

#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/bsc_sccp_backend.h>

struct osmo_ss7_instance;
struct bsc_sccp_inst;
struct aworker;

#define AWORKER_POOL_MAX_WORKERS	16
#define AWORKER_STARTUP_TIMEOUT_DEFAULT	20
#define AWORKER_RESTART_DELAY_DEFAULT	5

/* Exit codes of a worker process, as seen by the main process through waitpid(). */
#define AWORKER_EXIT_OK			0
#define AWORKER_EXIT_CONFIG		66	/*!< unusable worker configuration; a restart cannot help */
#define AWORKER_EXIT_LINK		67	/*!< the IPC link to the main process went down */

enum aworker_failure_policy {
	AWORKER_FAILURE_RESTART = 0,
	AWORKER_FAILURE_SHUTDOWN,
};

enum aworker_state {
	AWORKER_ST_STOPPED,	/*!< not running (before the first start, or after a fatal failure) */
	AWORKER_ST_STARTING,	/*!< process started, IPC handshake and/or CS7_INFO still pending */
	AWORKER_ST_READY,	/*!< handshake done and cs7 instances reported */
	AWORKER_ST_RESTARTING,	/*!< waiting for 'restart-delay' before the next start */
};

struct aworker_pool_cfg {
	/*! Number of workers; 0 disables worker mode (the default: osmo-bsc then runs the SIGTRAN stack
	 * in-process, exactly as without this feature). */
	unsigned int num_workers;
	/*! Config file of each worker; cfg_file[i] must be set for every i < num_workers. */
	char *cfg_file[AWORKER_POOL_MAX_WORKERS];
	unsigned int startup_timeout;	/*!< seconds to wait for all workers to become ready */
	unsigned int restart_delay;	/*!< seconds between a worker's death and its restart */
	enum aworker_failure_policy on_failure;
};

/* Configuration and VTY */
struct aworker_pool_cfg *aworker_pool_cfg(void);
bool aworker_pool_enabled(void);
void aworker_pool_vty_init(void);
/*! Scan the main configuration file for the 'a-worker-pool' section and, if worker mode is configured there,
 * refuse any 'asp', 'as' or 'listen' node inside a 'cs7 instance': reading those would create ASPs and open SCTP
 * associations in the main process, which the workers own. This runs BEFORE the configuration file is parsed, so
 * that such an ASP is never created in the first place.
 * \returns 0 if the file may be parsed, -EINVAL if it must not be (the reason is printed to stderr and logged),
 *	    -errno if the file cannot be read. */
int aworker_pool_prescan_config(const char *config_file);
/*! Second line of defence, run after the configuration file was parsed: refuse a main-process cs7 instance that
 * has an Application Server. \returns 0 or -EINVAL (errors are logged). */
int aworker_pool_check_main_cs7(void);
/*! Record how to start a worker: the osmo-bsc executable and the command line it was started with (the logging
 * options are passed on to the workers). Must be called before aworker_pool_start(). */
void aworker_pool_set_exec(const char *argv0, int argc, char **argv);

/* Life cycle, main process */

/*! Start all configured workers: one socketpair(AF_UNIX, SOCK_SEQPACKET), one fork() and one execv() of the
 * osmo-bsc executable each. The worker is exec'd rather than only forked so that it never inherits any state or
 * file descriptor of the main process -- which matters in particular for a worker restarted while the main
 * process is fully up and has its Abis, MGW, CTRL and VTY sockets open.
 * \returns 0 if all workers were started, negative errno otherwise (nothing is left running on error). */
int aworker_pool_start(void *ctx);
/*! Send GOODBYE to all workers, wait for them to exit and free the pool. Idempotent. */
void aworker_pool_stop(void);
bool aworker_pool_all_ready(void);
/*! True if the pool requested the shutdown of osmo-bsc because a worker could not be kept running. main()
 * returns a failure exit code in that case, so that a service manager sees it as a failure. */
bool aworker_pool_shutdown_was_fatal(void);
/*! What the worker that owns cs7 instance \a cs7_id reported about it in CS7_INFO.
 * \returns true and fills *out, or false if no ready worker owns that instance. */
bool aworker_pool_cs7_info(uint32_t cs7_id, struct aworker_ipc_cs7_entry *out);
void aworker_pool_dump_vty(struct vty *vty);

/*! Testing seam: start the pool on already connected AF_UNIX SOCK_SEQPACKET sockets instead of forking. fds[i]
 * is the main-process end of the link to worker i and is owned by the pool afterwards. */
int aworker_pool_start_with_fds(void *ctx, const int *fds, unsigned int num);

/* SCCP Users bound through a worker */

struct aworker_sccp_user_ops {
	/*! The worker could not execute a TX_* request. See the TX_FAILED contract in aworker_ipc.h: an operation
	 * with disposition EXECUTED_ERROR may or may not have reached the network and must not be replayed. */
	void (*tx_failed)(struct bsc_sccp_user *user, const struct aworker_ipc_tx_failed *f);
	/*! The user can no longer be used: the worker is gone or the bind failed. All SCCP connections of this
	 * user must be considered lost. Called at most once per 'available'. */
	void (*unavailable)(struct bsc_sccp_user *user);
	/*! The user is bound in its worker and can be used. */
	void (*available)(struct bsc_sccp_user *user);
};

/*! Bind an SCCP user in the worker that owns cs7 instance \a ss7. The bind is carried out as soon as that worker
 * is known and ready; until then, and whenever the worker is lost, transmit operations on the returned user fail
 * with -ENOTCONN and aworker_sccp_user_ready() is false.
 * \param[in] ctx talloc context of the returned user.
 * \param[in] ss7 cs7 instance (owned by a worker) this user belongs to.
 * \param[in] bsc_sccp the conn_id registry of that cs7 instance, kept by the main process.
 * \returns the new user, or NULL on allocation failure or if worker mode is not running. */
struct bsc_sccp_user *aworker_sccp_user_bind(void *ctx, struct osmo_ss7_instance *ss7,
					     struct bsc_sccp_inst *bsc_sccp, const char *name,
					     bsc_sccp_user_prim_cb prim_cb, uint16_t ssn,
					     const struct aworker_sccp_user_ops *ops);
/*! Find a user bound with aworker_sccp_user_bind() on this cs7 instance and SSN, or NULL. */
struct bsc_sccp_user *aworker_sccp_user_find(uint32_t cs7_id, uint16_t ssn);
void aworker_sccp_user_unbind(struct bsc_sccp_user *user);
bool aworker_sccp_user_ready(const struct bsc_sccp_user *user);

/*! The worker-backed SCCP User backend. */
extern const struct bsc_sccp_backend_ops bsc_sccp_backend_worker_ops;

extern const struct value_string aworker_failure_policy_names[];

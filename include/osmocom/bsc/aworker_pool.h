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
 * 'on-worker-failure restart' then restarts the worker after a delay; 'shutdown' terminates osmo-bsc.
 *
 * The restart delay backs off exponentially per worker: 'restart-delay' after the first unplanned loss, then twice
 * the previous delay after every further one, never more than 'restart-delay-max' (but never less than
 * 'restart-delay'). A worker that was READY for at least 'stable-time' before it was lost starts over at
 * 'restart-delay'. Time a worker spends starting (not yet READY) never counts as stable.
 *
 * 'max-restarts N window S' limits the restarts of each worker: the times of its unplanned restarts are recorded
 * (CLOCK_MONOTONIC, when the lost process is collected and the restart is decided). An unplanned loss that would be
 * the N-th restart within S seconds of the oldest restart still recorded (one is dropped once it is S seconds old)
 * puts the worker into FAILED (reason restart-limit) instead, so at most N-1 restarts happen within any S seconds.
 * Stability does not clear the record (it only resets the backoff); an operator reset does. Defaults: N 5, S 300.
 * Only unplanned losses count: config and ownership failures are FAILED at once, and planned stops never count.
 */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_ipc.h>

struct aworker;

#define AWORKER_POOL_MAX_WORKERS	16
#define AWORKER_STARTUP_TIMEOUT_DEFAULT	60
#define AWORKER_RESTART_DELAY_DEFAULT	5
#define AWORKER_RESTART_DELAY_MAX_DEFAULT	60
#define AWORKER_STABLE_TIME_DEFAULT	60
#define AWORKER_MAX_RESTARTS_DEFAULT	5
#define AWORKER_RESTART_WINDOW_DEFAULT	300
/* Session ECHO between BSC and Supervisor (D-S12, initial values decided for S2-5; to be confirmed by S2-8). */
#define AWORKER_SESSION_ECHO_INTERVAL_DEFAULT	2
/* sizeof(((struct sockaddr_un *)0)->sun_path) on Linux: a socket path must be shorter (its NUL included). */
#define AWORKER_SOCKET_PATH_MAX	108
#define AWORKER_SESSION_ECHO_MISSES_DEFAULT	3
#define AWORKER_MAX_RESTARTS_LIMIT	32	/*!< largest 'max-restarts' */

/* Taking a worker down never blocks the main process: SIGTERM, then waitpid(WNOHANG) from a timer, SIGKILL after
 * the grace period, and polling again until the process is collected. A worker whose process has not been collected
 * is never started again (see AWORKER_ST_TERMINATING). */
#define AWORKER_TERM_GRACE_MS		1000	/*!< SIGTERM -> SIGKILL */
#define AWORKER_REAP_POLL_MS		50	/*!< first waitpid(WNOHANG) poll interval; doubles up to the max */
#define AWORKER_REAP_POLL_MAX_MS	1000
#define AWORKER_REAP_STUCK_MS		10000	/*!< after SIGKILL: report the process as stuck (e.g. in D state) */

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
	AWORKER_ST_RESTARTING,	/*!< waiting for its restart delay (see FAILURE POLICY) before the next start */
	AWORKER_ST_TERMINATING,	/*!< lost: the process was signalled and has not been collected yet. A worker never
				 *   leaves this state before waitpid() returned its status, so that two generations of
				 *   one worker can never run at the same time. */
	AWORKER_ST_FAILED,	/*!< a failure that restarting cannot fix (see enum aworker_fail_reason): not restarted
				 *   until an operator resets it (aworker_pool_reset_worker(), VTY 'a-worker-pool reset
				 *   worker N'). Only this worker is affected: osmo-bsc and the other workers go on. */
};

/*! Why a worker is AWORKER_ST_FAILED. */
enum aworker_fail_reason {
	AWORKER_FAIL_NONE = 0,
	AWORKER_FAIL_CONFIG,		/*!< the worker exited with AWORKER_EXIT_CONFIG: it cannot use its config file */
	AWORKER_FAIL_OWNERSHIP,		/*!< it claimed a cs7 instance its config file did not have at start */
	AWORKER_FAIL_RESTART_LIMIT,	/*!< lost again although 'max-restarts' was reached ('max-restarts N window S') */
};
const char *aworker_fail_reason_name(enum aworker_fail_reason reason);
const char *aworker_state_name(enum aworker_state state);

/*! A read-only snapshot of one worker, for status reporting and tests. */
struct aworker_pool_worker_info {
	unsigned int idx;
	enum aworker_state state;
	pid_t pid;			/*!< current process; 0 if none (not started, or collected) */
	unsigned int starts;		/*!< number of processes started for this worker so far */
	const char *cfg_file;		/*!< the config file the pool starts this worker with (the pool's own copy;
					 *   valid until the pool stops) */
	bool sigkill_sent;		/*!< TERMINATING: SIGTERM was not enough */
	bool reap_stuck;		/*!< TERMINATING: still not collected AWORKER_REAP_STUCK_MS after SIGKILL */
	enum aworker_fail_reason fail_reason;	/*!< FAILED: why */
	const char *fail_detail;	/*!< FAILED: what happened, for the operator ("" otherwise; valid until the pool
					 *   stops) */
	time_t failed_at;		/*!< FAILED: when (wall clock); 0 otherwise */
	unsigned int backoff_step;	/*!< consecutive unplanned restarts since the worker was last stable (or reset):
					 *   the next restart waits restart-delay * 2^backoff_step, capped */
	unsigned int restart_delay;	/*!< seconds: delay of the restart scheduled last (0 if none so far) */
	unsigned int restarts_in_window; /*!< recorded unplanned restarts less than 'window' seconds old */
};

struct aworker_pool_cfg {
	/*! Number of workers; 0 disables worker mode (the default: osmo-bsc then runs the SIGTRAN stack
	 * in-process, exactly as without this feature). */
	unsigned int num_workers;
	/*! Config file of each worker (file form); every worker i < num_workers has either this or an inline
	 * cs7 instance, never both. */
	char *cfg_file[AWORKER_POOL_MAX_WORKERS];
	/*! Inline form: worker i owns 'cs7 instance cs7_instance[i]' of the main configuration file itself. */
	bool has_cs7_instance[AWORKER_POOL_MAX_WORKERS];
	uint8_t cs7_instance[AWORKER_POOL_MAX_WORKERS];
	unsigned int startup_timeout;	/*!< seconds each started worker process has to become READY */
	unsigned int restart_delay;	/*!< seconds: restart delay after the first unplanned loss (the backoff base) */
	unsigned int restart_delay_max;	/*!< seconds: the backoff never waits longer (but never less than restart_delay) */
	unsigned int stable_time;	/*!< seconds READY before a loss after which the backoff starts over */
	unsigned int max_restarts;	/*!< the max_restarts-th restart within restart_window makes the worker FAILED */
	unsigned int restart_window;	/*!< seconds */
	enum aworker_failure_policy on_failure;
	/*! The Supervisor's session socket (absolute path; N5), or NULL: no socket, no sessions (bsc-aworker as in
	 * S2-3). osmo-bsc parses it as part of the shared section; it does not use it yet (supervisor mode: S2-7). */
	char *supervisor_socket;
	unsigned int session_echo_interval;	/*!< seconds between session ECHO_REQs (D-S12) */
	unsigned int session_echo_misses;	/*!< consecutive misses that count as the threshold (D-S12) */
};

/* Configuration and VTY */
struct aworker_pool_cfg *aworker_pool_cfg(void);
bool aworker_pool_enabled(void);
void aworker_pool_vty_init(void *ctx);
/*! Scan the main configuration file for the 'a-worker-pool' section and, if worker mode is configured there,
 * refuse any 'asp', 'as' or 'listen' node inside a 'cs7 instance': reading those would create ASPs and open SCTP
 * associations in the main process, which the workers own. This runs BEFORE the configuration file is parsed, so
 * that such an ASP is never created in the first place.
 * \returns 0 if the file may be parsed, -EINVAL if it must not be (the reason is printed to stderr and logged),
 *	    -errno if the file cannot be read. */
int aworker_pool_prescan_config(const char *config_file);
/*! Parse the main configuration file. With the inline form ('worker <N> cs7-instance <X>'), only the BSC view is
 * parsed: the asp / as / listen nodes of the worker-owned cs7 instances are left out (see
 * aworker_cfg_view.h), the file is not recorded as the VTY's config file, and every way of writing the
 * configuration is refused from then on (aworker_pool_config_write_refusal()). Otherwise this is
 * vty_read_config_file(), exactly as before.
 * \returns 0, or a negative error as vty_read_config_file() returns it. */
int aworker_pool_read_config(const char *config_file);
/*! Why the running configuration must not be written to a file, or NULL if it may be. It must not once the main
 * configuration file was parsed as a view (the inline form): the BSC holds only its part of the configuration, so any
 * file written from it lacks the workers' nodes -- and written over osmo-bsc.cfg, it would delete them. */
const char *aworker_pool_config_write_refusal(void);
/*! Supervisor mode (bsc-aworker; decisions D-S1, D-S8, D-S10): read \a config_file once (aworker_cfg_read_stable()),
 * check it -- an 'a-worker-pool' with at least one worker, every worker in the inline form 'worker <N> cs7-instance
 * <X>' with its cs7 instance in this file, the rules of aworker_cfg_model_check() for supervisor mode -- and parse
 * only the Supervisor's view of it (the log targets and 'a-worker-pool'; nothing of the BSC, and no cs7 instance).
 * 'on-worker-failure shutdown' is refused: there is no BSC process for it to shut down. The pool started afterwards
 * (aworker_pool_start()) is run by the Supervisor: aworker_pool_supervisor_mode() is true from here on.
 * \returns 0, or -EINVAL with every problem printed to stderr and logged. */
int aworker_pool_supervisor_read_config(const char *config_file);
/*! Whether aworker_pool_supervisor_read_config() set up this process as the Supervisor. */
bool aworker_pool_supervisor_mode(void);
/*! For vty_app_info.config_is_consistent, which libosmovty asks before every 'write file [PATH]', 'write',
 * 'write memory' and 'copy running-config startup-config', and before it records PATH. Says why and \returns 0
 * if aworker_pool_config_write_refusal() refuses, 1 otherwise. */
struct vty;
int aworker_pool_vty_config_is_consistent(struct vty *vty);
/*! Second line of defence, run after the configuration file was parsed: refuse a main-process cs7 instance that
 * has an Application Server. \returns 0 or -EINVAL (errors are logged). */
int aworker_pool_check_main_cs7(void);
/*! 'cs7 instance <0-15>': the cs7 instance ids the expected-ownership map covers. */
#define AWORKER_POOL_MAX_CS7_ID		16
/*! An 'msc' of the main configuration and the cs7 instance it uses. */
struct aworker_pool_msc_ref {
	int nr;
	uint32_t cs7_id;
};
/*! Global consistency of the pool configuration, run after the main configuration was parsed and before any
 * worker is started. Reads the worker config files like the workers do (a worker owns the cs7 instances its file
 * configures with an asp, as or listen node) and refuses
 *  - a cs7 instance configured in two worker files, and
 *  - an msc whose cs7 instance no worker file configures (unless a worker file could not be read: then it may be
 *    in that file, and only an error is logged).
 * \returns 0, or -EINVAL (every problem is logged, naming the files, lines, instances and mscs). */
int aworker_pool_check_ownership(const struct aworker_pool_msc_ref *mscs, unsigned int num_mscs);
/*! The worker the running pool expects to own \a cs7_id, from its worker config files at start; -1 if none (or
 * no pool is running). */
int aworker_pool_expected_owner(uint32_t cs7_id);
/*! Record how to start a worker: the osmo-bsc executable and the command line it was started with (the logging
 * options are passed on to the workers). Must be called before aworker_pool_start(), early: it opens the running
 * executable (/proc/self/exe) once, and every worker generation is executed from that descriptor with fexecve(),
 * so a worker is always the same binary image as its main process, even if the file is replaced or deleted later. */
void aworker_pool_set_exec(const char *argv0, int argc, char **argv);

/* Life cycle, main process */

/*! Start all configured workers: one socketpair(AF_UNIX, SOCK_SEQPACKET), one fork() and one execv() of the
 * osmo-bsc executable each. The worker is exec'd rather than only forked so that it never inherits any state or
 * file descriptor of the main process -- which matters in particular for a worker restarted while the main
 * process is fully up and has its Abis, MGW, CTRL and VTY sockets open.
 * \returns 0 if all workers were started, negative errno otherwise (nothing is left running on error). */
int aworker_pool_start(void *ctx);
/*! Send GOODBYE to all workers, stop them and free the pool. Idempotent. Runs when osmo-bsc exits, after its
 * event loop: it polls the event loop (without blocking, at most 500 ms) until GOODBYE is written, gives the workers
 * the grace period to exit by themselves, then signals whatever is left and waits a bounded time for it (never
 * forever). A process that cannot be collected (stuck in the kernel) is reported, not assumed gone. Must not be
 * called from an osmo_select call-back. */
void aworker_pool_stop(void);
bool aworker_pool_all_ready(void);
/*! True if the pool requested the shutdown of osmo-bsc because a worker could not be kept running. main()
 * returns a failure exit code in that case, so that a service manager sees it as a failure. */
bool aworker_pool_shutdown_was_fatal(void);
/*! What the worker that owns cs7 instance \a cs7_id reported about it in CS7_INFO.
 * \returns true and fills *out, or false if no ready worker owns that instance. */
bool aworker_pool_cs7_info(uint32_t cs7_id, struct aworker_ipc_cs7_entry *out);
void aworker_pool_dump_vty(struct vty *vty);
/*! Number of workers of the running pool (0 if no pool is running). */
unsigned int aworker_pool_num_workers(void);
/*! Snapshot of worker \a idx of the running pool. \returns 0, or -ENODEV / -EINVAL. */
int aworker_pool_worker_info(unsigned int idx, struct aworker_pool_worker_info *out);
/*! Leave AWORKER_ST_FAILED: forget the failure and start the worker again (with its config file as read at
 * osmo-bsc start). Only that worker is touched.
 * \returns 0; -EBUSY if the worker is not FAILED (nothing is changed); -EINVAL for an unknown index; -ENODEV if
 *	    no pool runs; -ESHUTDOWN while the pool stops. */
int aworker_pool_reset_worker(unsigned int idx);

/*! Testing seam: start the pool on already connected AF_UNIX SOCK_SEQPACKET sockets instead of forking. fds[i]
 * is the main-process end of the link to worker i and is owned by the pool afterwards. */
int aworker_pool_start_with_fds(void *ctx, const int *fds, unsigned int num);

extern const struct value_string aworker_failure_policy_names[];

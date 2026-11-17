/* A-worker pool: internals shared by the process life cycle (libaworker) and the osmo-bsc SCCP user side */
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

/* Not a public API: the structures of the pool, used by the life cycle code in src/a-worker/aworker_pool.c and by
 * the osmo-bsc SCCP user side in src/osmo-bsc/aworker_users.c, which reaches the life cycle only through the hooks
 * below. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/timer.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

struct vty;
struct aworker_pool;

struct aworker {
	struct aworker_pool *pool;
	unsigned int idx;
	enum aworker_state state;
	pid_t pid;
	int main_fd;				/*!< main-process end of the socketpair; owned by link once set */
	struct aworker_ipc_link *link;
	struct osmo_timer_list restart_timer;
	unsigned int starts;			/*!< number of times this worker was started */
	/*! Startup timeout of the current process: armed when it enters STARTING (link_create(), every start),
	 * deleted on every way out of STARTING (READY, worker_lost(), aworker_pool_stop()). */
	struct osmo_timer_list start_timer;

	/* Restart backoff, see restart_delay_for(). All times are CLOCK_MONOTONIC (osmo_clock_gettime()). */
	unsigned int backoff_step;		/*!< consecutive unplanned restarts since last stable or reset */
	unsigned int restart_delay_s;		/*!< delay of the restart scheduled last */
	bool ready_seen;			/*!< the current process reached READY ... */
	struct timespec ready_since;		/*!< ... at this time */
	bool stable_at_loss;			/*!< it was READY for at least stable-time when it was lost */
	/*! Restart limit: times of the recorded unplanned restarts, oldest first, see worker_schedule_restart(). */
	struct timespec restart_at[AWORKER_MAX_RESTARTS_LIMIT];
	unsigned int num_restart_at;

	/* Taking the process down without blocking (AWORKER_ST_TERMINATING), see worker_begin_termination(). */
	struct osmo_timer_list reap_timer;
	struct timespec term_since;		/*!< when SIGTERM was sent */
	struct timespec kill_since;		/*!< when SIGKILL was sent */
	unsigned int reap_poll_ms;		/*!< next waitpid(WNOHANG) poll interval */
	bool sigkill_sent;
	bool reap_stuck;			/*!< not collected AWORKER_REAP_STUCK_MS after SIGKILL */
	pid_t last_pid;				/*!< the process that was lost, for the log after it is collected */
	char lost_reason[160];			/*!< why it was lost, applied with the failure policy once collected */

	/* AWORKER_ST_FAILED. A cause found while the process still runs is recorded in pending_fail (with its
	 * fail_detail) and applied by worker_apply_policy(), once the process is collected. */
	enum aworker_fail_reason pending_fail;
	enum aworker_fail_reason fail_reason;
	char fail_detail[200];
	time_t failed_at;

	/* Clean shutdown, see aworker_pool_stop(). */
	bool goodbye_queued;
	uint32_t goodbye_seq;			/*!< sequence number of the queued GOODBYE */
	bool goodbye_written;			/*!< ... and the kernel took it: the worker is exiting by itself */

	/*! cs7 instances this worker reported in CS7_INFO. */
	struct aworker_ipc_cs7_info cs7;
	bool cs7_reported;
};

/* Which worker is expected to own which cs7 instance, read from the worker config files with the worker's own rule
 * (aworker.c, cs7_collect()): a worker owns exactly the cs7 instances for which its file has an asp, as or listen
 * node. */
struct ownership {
	int owner[AWORKER_POOL_MAX_CS7_ID];		/*!< worker index, or -1: no file configures this instance */
	unsigned int lineno[AWORKER_POOL_MAX_CS7_ID];	/*!< line of its 'cs7 instance' command in that file */
	bool unreadable[AWORKER_POOL_MAX_WORKERS];	/*!< this worker's file could not be read: what it owns is unknown */
	unsigned int num_unreadable;
	/*! The exact set this worker owns is known: its file was read and configures at least one instance. (A
	 * worker whose file configures none refuses to run anyway.) Its CS7_INFO must match that set. */
	bool known[AWORKER_POOL_MAX_WORKERS];
};

struct aworker_pool {
	void *ctx;
	/*! Copy taken at start time. Its strings (cfg_file[]) are the pool's own talloc copies: the VTY may replace or
	 * free the ones in g_cfg at any time, and changes there only take effect when osmo-bsc is restarted. */
	struct aworker_pool_cfg cfg;
	unsigned int num;
	struct aworker *worker[AWORKER_POOL_MAX_WORKERS];
	struct llist_head users;
	uint32_t next_user_ref;
	struct osmo_timer_list startup_timer;
	struct ownership own;			/*!< expected ownership, built at start */
	bool no_fork;				/*!< started with aworker_pool_start_with_fds() */
	bool stopping;
};

#define LOGW(w, lvl, fmt, args...) \
	LOGP(aworker_log_cat, lvl, "aworker %u (pid %ld): " fmt, (w)->idx, (long)(w)->pid, ## args)

/* The worker that owns cs7 instance cs7_id, or NULL if no ready worker reported it. */
static inline struct aworker *worker_by_cs7(struct aworker_pool *pool, uint32_t cs7_id)
{
	unsigned int i, j;

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->state != AWORKER_ST_READY)
			continue;
		for (j = 0; j < w->cs7.count; j++) {
			if (w->cs7.e[j].cs7_id == cs7_id)
				return w;
		}
	}
	return NULL;
}

static inline const struct aworker_ipc_cs7_entry *cs7_entry(const struct aworker *w, uint32_t cs7_id)
{
	unsigned int j;

	for (j = 0; j < w->cs7.count; j++) {
		if (w->cs7.e[j].cs7_id == cs7_id)
			return &w->cs7.e[j];
	}
	return NULL;
}

/*! The running pool, or NULL. */
struct aworker_pool *aworker_pool_get(void);

/*! What the life cycle tells the SCCP user side, at the points where it did so when both were one file. Set once by
 * the user side (aworker_pool_set_user_hooks()); until then there are no users, and the life cycle behaves as the
 * user side does with none. */
struct aworker_pool_user_hooks {
	/*! A worker became READY (end of handle_cs7_info()). */
	void (*worker_ready)(struct aworker_pool *pool);
	/*! A worker was lost (worker_lost(), before its process is collected). */
	void (*worker_lost)(struct aworker *w);
	/*! USER_BIND_RESULT, SCU_UP or TX_FAILED received from a worker. */
	void (*rx)(struct aworker *w, const struct aworker_ipc_msg *m);
	/*! The pool's startup timer fired. */
	void (*startup_report)(struct aworker_pool *pool);
	/*! aworker_pool_stop(), after the processes are gone and before the pool is freed. */
	void (*pool_stopped)(struct aworker_pool *pool);
	/*! End of aworker_pool_dump_vty(). */
	void (*dump)(struct vty *vty, struct aworker_pool *pool);
};

void aworker_pool_set_user_hooks(const struct aworker_pool_user_hooks *hooks);

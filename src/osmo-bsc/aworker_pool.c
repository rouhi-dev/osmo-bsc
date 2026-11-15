/* A-worker pool: the osmo-bsc main process side (see the ownership model in aworker_pool.h) */
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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <limits.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/aworker_cfg_scan.h>
#include <osmocom/bsc/bsc_sccp_backend.h>
#include <osmocom/bsc/debug.h>

/* aworker_pool_stop() runs after the event loop has ended (osmo-bsc is exiting), so it may wait, but only this long
 * after SIGKILL for processes that are still not collected. */
#define STOP_KILL_WAIT_MS	2000
/* aworker_pool_stop(): how long to keep the event loop going for a queued GOODBYE to be written. */
#define STOP_FLUSH_MS		500
#define STOP_POLL_MS		20

extern char **environ;

struct aworker_pool;

/*! One SCCP User the main process keeps in a worker. */
struct aworker_user {
	struct llist_head entry;		/*!< in aworker_pool.users */
	struct aworker_pool *pool;
	uint32_t cs7_id;
	uint16_t ssn;
	char name[AWORKER_IPC_MAX_USER_NAME + 1];
	const struct aworker_sccp_user_ops *ops;
	struct bsc_sccp_user *user;		/*!< the generic user; owns nothing of this struct */

	/*! Worker this user is currently bound in, and the user_ref valid in it. Both are reset whenever the
	 * worker is lost, and a fresh user_ref is drawn on the next bind, so that no record of a previous worker
	 * (or of a previous bind) can ever be attributed to this user. */
	struct aworker *worker;
	uint32_t user_ref;
	bool bind_sent;
	bool bound;
};

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

static struct aworker_pool_cfg g_cfg = {
	.num_workers = 0,
	.startup_timeout = AWORKER_STARTUP_TIMEOUT_DEFAULT,
	.restart_delay = AWORKER_RESTART_DELAY_DEFAULT,
	.restart_delay_max = AWORKER_RESTART_DELAY_MAX_DEFAULT,
	.stable_time = AWORKER_STABLE_TIME_DEFAULT,
	.max_restarts = AWORKER_MAX_RESTARTS_DEFAULT,
	.restart_window = AWORKER_RESTART_WINDOW_DEFAULT,
	.on_failure = AWORKER_FAILURE_RESTART,
};

static struct aworker_pool *g_pool;

/* Set when the pool asked osmo-bsc to shut down because a worker could not be kept running. Outlives the pool. */
static bool g_fatal;

/* How a worker is started: the osmo-bsc executable and the logging options of our own command line. Set by
 * aworker_pool_set_exec(). */
static char *g_exec_path;
static char **g_exec_log_argv;		/*!< NULL terminated, talloc'ed under g_exec_path */
static unsigned int g_exec_log_argc;
/*! The running executable, opened once by aworker_pool_set_exec() (O_CLOEXEC). Workers are fexecve()d from it, so
 * every generation is the very binary image of the main process, whatever happens to its path later. -1: not
 * available, fall back to g_exec_path. */
static int g_exec_fd = -1;

#define LOGW(w, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker %u (pid %ld): " fmt, (w)->idx, (long)(w)->pid, ## args)
#define LOGU(au, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker user '%s' (cs7 %u ssn %u): " fmt, (au)->name, (au)->cs7_id, (au)->ssn, ## args)

static const struct value_string state_names[] = {
	{ AWORKER_ST_STOPPED, "STOPPED" },
	{ AWORKER_ST_STARTING, "STARTING" },
	{ AWORKER_ST_READY, "READY" },
	{ AWORKER_ST_RESTARTING, "RESTARTING" },
	{ AWORKER_ST_TERMINATING, "TERMINATING" },
	{ AWORKER_ST_FAILED, "FAILED" },
	{ 0, NULL }
};

static const struct value_string fail_reason_names[] = {
	{ AWORKER_FAIL_NONE, "none" },
	{ AWORKER_FAIL_CONFIG, "config" },
	{ AWORKER_FAIL_OWNERSHIP, "ownership" },
	{ AWORKER_FAIL_RESTART_LIMIT, "restart-limit" },
	{ 0, NULL }
};

const char *aworker_fail_reason_name(enum aworker_fail_reason reason)
{
	return get_value_string(fail_reason_names, reason);
}

const struct value_string aworker_failure_policy_names[] = {
	{ AWORKER_FAILURE_RESTART, "restart" },
	{ AWORKER_FAILURE_SHUTDOWN, "shutdown" },
	{ 0, NULL }
};

/***********************************************************************
 * Configuration
 ***********************************************************************/

struct aworker_pool_cfg *aworker_pool_cfg(void)
{
	return &g_cfg;
}

bool aworker_pool_enabled(void)
{
	return g_cfg.num_workers > 0;
}

/* Refuse a main configuration file that would make this process own a SIGTRAN stack. The check runs before the
 * file is parsed, because parsing an 'asp' node is what creates the ASP and opens its SCTP association; see
 * aworker_cfg_scan.h. It only ever rejects -- the VTY parser remains the single source of the configuration, and
 * a file without an 'a-worker-pool' section is not restricted at all. */
int aworker_pool_prescan_config(const char *config_file)
{
	const struct aworker_cfg_scan_cs7 *c;
	struct aworker_cfg_scan scan;
	int rc;

	rc = aworker_cfg_scan(config_file, &scan);
	if (rc < 0) {
		/* Leave reporting an unreadable file to vty_read_config_file(), which says it the way it always
		 * has. There is nothing to restrict if we cannot read the file anyway. */
		return 0;
	}
	if (!scan.num_workers)
		return 0;
	c = aworker_cfg_scan_first_sigtran(&scan);
	if (!c)
		return 0;

	/* Logging is not configured yet at this point, so say it on stderr as well. */
	fprintf(stderr,
		"%s:%u: '%s' is not allowed inside 'cs7 instance %u' (line %u) of the main configuration file"
		" while 'a-worker-pool' configures %u worker(s).\n"
		"Reading an asp / as / listen node creates the ASP and opens its SCTP association in the osmo-bsc"
		" main process, but the A-workers own the SIGTRAN stack.\n"
		"Move the asp, as and listen nodes into the worker config file(s) named by"
		" 'worker <N> config-file FILE', and keep only 'point-code', 'point-code format' and the"
		" 'sccp-address' book in the main configuration file.\n",
		config_file, c->node_lineno, c->node, c->id, c->lineno, scan.num_workers);
	LOGP(DAWORKER, LOGL_FATAL,
	     "%s:%u: '%s' is not allowed inside 'cs7 instance %u' (line %u) when 'a-worker-pool' is configured\n",
	     config_file, c->node_lineno, c->node, c->id, c->lineno);
	return -EINVAL;
}

/***********************************************************************
 * Expected ownership of the cs7 instances
 ***********************************************************************/

/* Scan the config files of the first cfg->num_workers workers into \a own. A cs7 instance configured in two of them is
 * a contradiction in the pool configuration itself: logged naming both files, \returns -EINVAL. A file that cannot
 * be read is only recorded in own->unreadable[]: that is the worker's own problem, reported when it starts. */
static int ownership_build(const struct aworker_pool_cfg *cfg, struct ownership *own)
{
	unsigned int i, j;
	int rc = 0;

	memset(own, 0, sizeof(*own));
	for (j = 0; j < AWORKER_POOL_MAX_CS7_ID; j++)
		own->owner[j] = -1;

	for (i = 0; i < cfg->num_workers && i < AWORKER_POOL_MAX_WORKERS; i++) {
		struct aworker_cfg_scan scan;
		int src;

		if (!cfg->cfg_file[i])
			continue;
		src = aworker_cfg_scan(cfg->cfg_file[i], &scan);
		if (src < 0) {
			LOGP(DAWORKER, LOGL_ERROR, "aworker %u: cannot read its config file '%s': %s; which cs7"
			     " instances it owns is unknown\n", i, cfg->cfg_file[i], strerror(-src));
			own->unreadable[i] = true;
			own->num_unreadable++;
			continue;
		}
		for (j = 0; j < scan.num_cs7; j++) {
			const struct aworker_cfg_scan_cs7 *sc = &scan.cs7[j];
			int prev;

			if (!sc->num_asp && !sc->num_as && !sc->num_listen)
				continue;	/* configuration only: nothing to own (as in the worker) */
			if (sc->id >= AWORKER_POOL_MAX_CS7_ID) {
				/* Not a valid 'cs7 instance' number; the worker's VTY parser refuses the file. */
				continue;
			}
			prev = own->owner[sc->id];
			if (prev >= 0 && prev != (int)i) {
				LOGP(DAWORKER, LOGL_FATAL, "'a-worker-pool': cs7 instance %u is configured in two worker"
				     " config files: worker %d '%s' (line %u) and worker %u '%s' (line %u). Each cs7"
				     " instance must be configured, with its asp / as, in exactly one worker config"
				     " file.\n", sc->id, prev, cfg->cfg_file[prev], own->lineno[sc->id], i,
				     cfg->cfg_file[i], sc->lineno);
				rc = -EINVAL;
				continue;
			}
			if (prev < 0) {
				own->owner[sc->id] = i;
				own->lineno[sc->id] = sc->lineno;
			}
			own->known[i] = true;
		}
	}
	return rc;
}

int aworker_pool_check_ownership(const struct aworker_pool_msc_ref *mscs, unsigned int num_mscs)
{
	struct ownership own;
	unsigned int i;
	int rc;

	if (!aworker_pool_enabled())
		return 0;

	rc = ownership_build(&g_cfg, &own);

	for (i = 0; i < num_mscs; i++) {
		uint32_t id = mscs[i].cs7_id;

		if (id < AWORKER_POOL_MAX_CS7_ID && own.owner[id] >= 0)
			continue;
		if (own.num_unreadable) {
			/* It may be in a file we could not read: that worker fails on its own when it starts. */
			LOGP(DAWORKER, LOGL_ERROR, "'a-worker-pool': msc %d uses cs7 instance %u, which none of the"
			     " readable worker config files configures (%u could not be read)\n", mscs[i].nr, id,
			     own.num_unreadable);
			continue;
		}
		LOGP(DAWORKER, LOGL_FATAL, "'a-worker-pool': msc %d uses cs7 instance %u, but no worker config file"
		     " configures that instance with an asp / as, so no A-worker would ever serve this msc. Add"
		     " 'cs7 instance %u' with its asp to exactly one worker config file.\n", mscs[i].nr, id, id);
		rc = -EINVAL;
	}
	return rc;
}

/* After the configuration was parsed: no cs7 instance of the main process may have an Application Server. This
 * catches an asp / as that the pre-scan did not see (an unusual spelling, a config built over the VTY, ...).
 * osmo_ss7_as_find_by_proto() is the only enumeration the public libosmo-sigtran API offers. */
int aworker_pool_check_main_cs7(void)
{
	struct llist_head *ll_it;
	int rc = 0;

	if (!aworker_pool_enabled())
		return 0;

	llist_for_each(ll_it, &osmo_ss7_instances) {
		struct osmo_ss7_instance *inst = osmo_ss7_instances_llist_entry(ll_it);
		uint32_t id = osmo_ss7_instance_get_id(inst);
		enum osmo_ss7_asp_protocol p;

		for (p = OSMO_SS7_ASP_PROT_SUA; p < _NUM_OSMO_SS7_ASP_PROT; p++) {
			if (!osmo_ss7_as_find_by_proto(inst, p))
				continue;
			LOGP(DAWORKER, LOGL_FATAL,
			     "'a-worker-pool' is configured, but 'cs7 instance %u' of the main process has the"
			     " %s Application Server. The A-workers own the SIGTRAN stack; the main process"
			     " must only configure 'point-code', 'point-code format' and the 'sccp-address' book."
			     " See 'show cs7 config'.\n",
			     id, osmo_ss7_asp_protocol_name(p));
			rc = -EINVAL;
		}
	}
	return rc;
}

/***********************************************************************
 * Lookups
 ***********************************************************************/

/* The worker that owns cs7 instance cs7_id, or NULL if no ready worker reported it. */
static struct aworker *worker_by_cs7(struct aworker_pool *pool, uint32_t cs7_id)
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

static const struct aworker_ipc_cs7_entry *cs7_entry(const struct aworker *w, uint32_t cs7_id)
{
	unsigned int j;

	for (j = 0; j < w->cs7.count; j++) {
		if (w->cs7.e[j].cs7_id == cs7_id)
			return &w->cs7.e[j];
	}
	return NULL;
}

/* A user of this worker by the user_ref we assigned to it. Only refs of the current bind match. */
static struct aworker_user *user_by_ref(struct aworker *w, uint32_t user_ref)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &w->pool->users, entry) {
		if (au->worker == w && au->bind_sent && au->user_ref == user_ref)
			return au;
	}
	return NULL;
}

static struct aworker_user *user_by_cs7_ssn(struct aworker_pool *pool, uint32_t cs7_id, uint16_t ssn)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry) {
		if (au->cs7_id == cs7_id && au->ssn == ssn)
			return au;
	}
	return NULL;
}

/***********************************************************************
 * User state transitions
 ***********************************************************************/

static void user_set_unavailable(struct aworker_user *au)
{
	bool was_bound = au->bound;

	au->worker = NULL;
	au->bind_sent = false;
	au->bound = false;
	if (was_bound && au->ops && au->ops->unavailable)
		au->ops->unavailable(au->user);
}

/* Queue a USER_BIND for this user in its worker. */
static void user_try_bind(struct aworker_user *au)
{
	struct aworker_pool *pool = au->pool;
	struct aworker *w;
	struct msgb *rec;
	uint32_t seq;
	int rc;

	if (au->bind_sent || pool->stopping)
		return;
	w = worker_by_cs7(pool, au->cs7_id);
	if (!w)
		return;

	/* A fresh user_ref per bind: records of an earlier bind or of a dead worker can never match. */
	au->user_ref = pool->next_user_ref++;
	if (!pool->next_user_ref)
		pool->next_user_ref = 1;
	au->worker = w;

	rc = aworker_ipc_enc_user_bind(pool->ctx, &rec, au->user_ref, au->cs7_id, au->ssn, au->name);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode USER_BIND: %s\n", strerror(-rc));
		au->worker = NULL;
		return;
	}
	rc = aworker_ipc_link_send(w->link, rec, &seq);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot send USER_BIND to worker %u: %s\n", w->idx, strerror(-rc));
		au->worker = NULL;
		return;
	}
	au->bind_sent = true;
	LOGU(au, LOGL_INFO, "USER_BIND sent to worker %u (user_ref %u, seq %u)\n", w->idx, au->user_ref, seq);
}

static void users_try_bind_all(struct aworker_pool *pool)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry)
		user_try_bind(au);
}

/* Every user that was bound in, or waiting for, this worker loses it. */
static void users_lose_worker(struct aworker *w)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &w->pool->users, entry) {
		if (au->worker == w)
			user_set_unavailable(au);
	}
}

/***********************************************************************
 * Worker life cycle
 ***********************************************************************/

static int worker_spawn(struct aworker *w);
static void worker_lost(struct aworker *w, const char *why);
static void start_timer_cb(void *data);

static unsigned int ms_since(const struct timespec *t)
{
	struct timespec now;
	long long ms;

	osmo_clock_gettime(CLOCK_MONOTONIC, &now);
	ms = (long long)(now.tv_sec - t->tv_sec) * 1000 + (now.tv_nsec - t->tv_nsec) / 1000000;
	return ms < 0 ? 0 : (ms > UINT_MAX ? UINT_MAX : (unsigned int)ms);
}

static void log_exit_status(struct aworker *w, int status)
{
	if (WIFEXITED(status))
		LOGW(w, WEXITSTATUS(status) == AWORKER_EXIT_OK ? LOGL_INFO : LOGL_NOTICE,
		     "exited with status %u\n", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		LOGW(w, LOGL_ERROR, "killed by signal %u\n", WTERMSIG(status));
}

/* A failure that restarting cannot fix: keep the worker FAILED until an operator resets it. Only this worker. */
static void worker_fail(struct aworker *w, enum aworker_fail_reason reason, const char *detail)
{
	w->state = AWORKER_ST_FAILED;
	w->fail_reason = reason;
	osmo_strlcpy(w->fail_detail, detail, sizeof(w->fail_detail));
	w->failed_at = time(NULL);
	osmo_timer_del(&w->restart_timer);
	LOGP(DAWORKER, LOGL_ERROR, "aworker %u (pid %ld): FAILED (%s): %s. It is not restarted; osmo-bsc and the"
	     " other workers continue. Fix the cause, then 'a-worker-pool reset worker %u'.\n", w->idx,
	     (long)w->last_pid, aworker_fail_reason_name(reason), w->fail_detail, w->idx);
}

/* Restart backoff: the delay before the restart after the step-th consecutive unplanned loss (step 0: the first).
 * restart-delay * 2^step, but at most restart-delay-max -- and never less than restart-delay, so that a configured
 * restart-delay above restart-delay-max keeps its meaning. A restart-delay of 0 stays 0. */
static unsigned int restart_delay_for(const struct aworker_pool_cfg *cfg, unsigned int step)
{
	unsigned int cap = OSMO_MAX(cfg->restart_delay, cfg->restart_delay_max);
	unsigned int d = cfg->restart_delay;

	while (step-- && d && d < cap)
		d *= 2;
	return OSMO_MIN(d, cap);
}

/* Is the worker's current process stable: READY for at least stable-time? Time before READY never counts. */
static bool worker_is_stable(const struct aworker *w)
{
	return w->state == AWORKER_ST_READY && w->ready_seen
	       && ms_since(&w->ready_since) >= (unsigned long long)w->pool->cfg.stable_time * 1000;
}

/* The backoff step the next restart would use, were the worker lost now. */
static unsigned int worker_next_step(const struct aworker *w)
{
	return worker_is_stable(w) ? 0 : w->backoff_step;
}

/* Recorded restarts within the window: one counts while it is less than restart-window seconds old. */
static bool restart_in_window(const struct aworker *w, unsigned int i)
{
	return ms_since(&w->restart_at[i]) < (unsigned long long)w->pool->cfg.restart_window * 1000;
}

static unsigned int restarts_in_window(const struct aworker *w)
{
	unsigned int i, n = 0;

	for (i = 0; i < w->num_restart_at; i++)
		n += restart_in_window(w, i);
	return n;
}

/* Drop the recorded restarts that left the window (the oldest are first). */
static void restarts_expire(struct aworker *w)
{
	unsigned int i = 0;

	while (i < w->num_restart_at && !restart_in_window(w, i))
		i++;
	memmove(&w->restart_at[0], &w->restart_at[i], (w->num_restart_at - i) * sizeof(w->restart_at[0]));
	w->num_restart_at -= i;
}

/* An unplanned loss with 'on-worker-failure restart', decided when the process is collected: unless this would be
 * the max-restarts-th restart within restart-window (counting from the oldest recorded one still in it), record it
 * and schedule the restart, backing off. Otherwise the worker is FAILED (restart-limit): with the defaults, at most
 * 4 restarts within 300 s. */
static void worker_schedule_restart(struct aworker *w)
{
	struct aworker_pool *pool = w->pool;

	restarts_expire(w);
	if (w->num_restart_at + 1 >= pool->cfg.max_restarts) {
		char detail[sizeof(w->fail_detail)];

		/* The loss reason last and bounded: the limit is what the operator needs to see. */
		if (w->num_restart_at)
			snprintf(detail, sizeof(detail), "lost again after %u restarts within %u s (the first %u s"
				 " ago): restart %u would reach 'max-restarts %u window %u'; last loss: %.60s",
				 w->num_restart_at, pool->cfg.restart_window, ms_since(&w->restart_at[0]) / 1000,
				 w->num_restart_at + 1, pool->cfg.max_restarts, pool->cfg.restart_window,
				 w->lost_reason);
		else
			snprintf(detail, sizeof(detail), "lost: 'max-restarts %u window %u' allows no automatic"
				 " restart; last loss: %.60s", pool->cfg.max_restarts, pool->cfg.restart_window,
				 w->lost_reason);
		w->stable_at_loss = false;
		worker_fail(w, AWORKER_FAIL_RESTART_LIMIT, detail);
		return;
	}
	OSMO_ASSERT(w->num_restart_at < ARRAY_SIZE(w->restart_at));
	osmo_clock_gettime(CLOCK_MONOTONIC, &w->restart_at[w->num_restart_at++]);

	if (w->stable_at_loss && w->backoff_step) {
		LOGP(DAWORKER, LOGL_NOTICE, "aworker %u: it was READY for at least stable-time (%u s) before it was"
		     " lost: the restart delay starts over at restart-delay\n", w->idx, pool->cfg.stable_time);
		w->backoff_step = 0;
	}
	w->stable_at_loss = false;
	w->restart_delay_s = restart_delay_for(&pool->cfg, w->backoff_step);
	if (w->backoff_step < 32)
		w->backoff_step++;
	LOGP(DAWORKER, LOGL_ERROR, "aworker %u (pid %ld): worker lost (%s); restarting in %u s (unplanned restart"
	     " %u in a row)\n", w->idx, (long)w->last_pid, w->lost_reason, w->restart_delay_s, w->backoff_step);
	w->state = AWORKER_ST_RESTARTING;
	osmo_timer_schedule(&w->restart_timer, w->restart_delay_s, 0);
}

/* A start that failed before there was a process (socketpair(), fork()): try again after the current delay. This
 * is no loss of a running worker, so the backoff does not advance; at least 1 s, so that it is never a busy loop. */
static void worker_retry_start(struct aworker *w)
{
	unsigned int delay = OSMO_MAX(w->restart_delay_s, 1);

	LOGW(w, LOGL_ERROR, "start failed, retrying in %u s\n", delay);
	w->state = AWORKER_ST_RESTARTING;
	osmo_timer_schedule(&w->restart_timer, delay, 0);
}

/* What happens to a lost worker once its process is collected (or if there never was one): the failure policy. */
static void worker_apply_policy(struct aworker *w, bool config_error)
{
	struct aworker_pool *pool = w->pool;
	const char *why = w->lost_reason;
	long pid = (long)w->last_pid;
	enum aworker_fail_reason fail = config_error ? AWORKER_FAIL_CONFIG : w->pending_fail;
	char detail[sizeof(w->fail_detail)];

	w->pending_fail = AWORKER_FAIL_NONE;
	if (pool->stopping)
		return;

	if (fail == AWORKER_FAIL_CONFIG)
		snprintf(detail, sizeof(detail), "exited with status %u: it cannot use its config file '%s' (its own"
			 " log says why)", AWORKER_EXIT_CONFIG, pool->cfg.cfg_file[w->idx]);
	else if (fail != AWORKER_FAIL_NONE)
		osmo_strlcpy(detail, w->fail_detail, sizeof(detail));

	/* Started on existing sockets (aworker_pool_start_with_fds()): there is no process to restart and no
	 * configuration of our own to blame, so just leave the worker stopped. */
	if (pool->no_fork) {
		if (fail != AWORKER_FAIL_NONE) {
			worker_fail(w, fail, detail);
			return;
		}
		LOGP(DAWORKER, LOGL_ERROR, "aworker %u: worker lost (%s)\n", w->idx, why);
		return;
	}

	switch (pool->cfg.on_failure) {
	case AWORKER_FAILURE_SHUTDOWN:
		/* The explicit opt-in: any worker failure ends osmo-bsc, including one restarting cannot fix. */
		LOGP(DAWORKER, LOGL_FATAL, "aworker %u (pid %ld): worker lost (%s) and 'on-worker-failure"
		     " shutdown' is configured; shutting down osmo-bsc\n", w->idx, pid,
		     fail != AWORKER_FAIL_NONE ? detail : why);
		g_fatal = true;
		osmo_select_shutdown_request();
		return;
	case AWORKER_FAILURE_RESTART:
	default:
		if (fail != AWORKER_FAIL_NONE) {
			worker_fail(w, fail, detail);
			return;
		}
		worker_schedule_restart(w);
		return;
	}
}

/* The process is gone and collected: only now may this worker be started again. */
static void worker_collected(struct aworker *w, int status, bool have_status)
{
	bool config_error = have_status && WIFEXITED(status) && WEXITSTATUS(status) == AWORKER_EXIT_CONFIG;

	osmo_timer_del(&w->reap_timer);
	if (have_status)
		log_exit_status(w, status);
	w->last_pid = w->pid;
	w->pid = 0;
	w->sigkill_sent = false;
	w->reap_stuck = false;
	w->state = AWORKER_ST_STOPPED;
	worker_apply_policy(w, config_error);
}

/* One non-blocking look at the process. \returns true if it was collected (worker_collected() has run). */
static bool worker_try_collect(struct aworker *w)
{
	int status;
	pid_t rc;

	rc = waitpid(w->pid, &status, WNOHANG);
	if (rc == w->pid) {
		worker_collected(w, status, true);
		return true;
	}
	if (rc < 0 && errno == ECHILD) {
		/* A child stays waitable until it is reaped, so for our own former child ECHILD means it is dead and
		 * was already reaped (e.g. by the kernel, if SIGCHLD is ignored). Its pid may already belong to another
		 * process: never look at or signal that pid again. */
		LOGW(w, LOGL_NOTICE, "process already reaped elsewhere (exit status unknown)\n");
		worker_collected(w, 0, false);
		return true;
	}
	if (rc < 0 && errno != EINTR) {
		LOGW(w, LOGL_ERROR, "waitpid() failed: %s; will try again\n", strerror(errno));
	}
	return false;
}

/* Timer: poll the process, escalate to SIGKILL after the grace period, report it if it cannot be collected. Never
 * blocks; the interval grows from AWORKER_REAP_POLL_MS to AWORKER_REAP_POLL_MAX_MS, so this is no busy loop. */
static void reap_timer_cb(void *data)
{
	struct aworker *w = data;
	unsigned int next;

	if (worker_try_collect(w))
		return;

	if (!w->sigkill_sent) {
		unsigned int elapsed = ms_since(&w->term_since);
		if (elapsed >= AWORKER_TERM_GRACE_MS) {
			LOGW(w, LOGL_ERROR, "did not exit %u ms after SIGTERM, sending SIGKILL\n", elapsed);
			kill(w->pid, SIGKILL);
			w->sigkill_sent = true;
			osmo_clock_gettime(CLOCK_MONOTONIC, &w->kill_since);
			w->reap_poll_ms = AWORKER_REAP_POLL_MS;
		}
	} else if (!w->reap_stuck && ms_since(&w->kill_since) >= AWORKER_REAP_STUCK_MS) {
		/* E.g. in uninterruptible sleep in the kernel. It cannot run any code of ours any more, but it is not
		 * gone either: keep it TERMINATING (so it is never started twice) and keep trying, less often. */
		w->reap_stuck = true;
		LOGW(w, LOGL_FATAL, "still not collected %u ms after SIGKILL (stuck in the kernel?); this worker is not"
		     " restarted until the process is gone\n", ms_since(&w->kill_since));
	}

	next = w->reap_poll_ms;
	if (!w->sigkill_sent) {
		/* Do not overshoot the grace period. */
		unsigned int elapsed = ms_since(&w->term_since);
		unsigned int left = elapsed < AWORKER_TERM_GRACE_MS ? AWORKER_TERM_GRACE_MS - elapsed : 0;
		if (left < next)
			next = left ? left : 1;
	}
	w->reap_poll_ms = OSMO_MIN(w->reap_poll_ms * 2, AWORKER_REAP_POLL_MAX_MS);
	osmo_timer_schedule(&w->reap_timer, next / 1000, (next % 1000) * 1000);
}

/* Take the worker's process down without blocking: collect it right away if it has already exited, otherwise
 * SIGTERM it and let reap_timer_cb() collect it (SIGKILL after AWORKER_TERM_GRACE_MS). The worker stays in
 * AWORKER_ST_TERMINATING until then; worker_collected() applies the failure policy. */
static void worker_begin_termination(struct aworker *w)
{
	OSMO_ASSERT(w->pid > 0);

	w->state = AWORKER_ST_TERMINATING;
	w->sigkill_sent = false;
	w->reap_stuck = false;
	if (worker_try_collect(w))
		return;

	kill(w->pid, SIGTERM);
	osmo_clock_gettime(CLOCK_MONOTONIC, &w->term_since);
	w->reap_poll_ms = AWORKER_REAP_POLL_MS;
	LOGW(w, LOGL_INFO, "SIGTERM sent; collecting the process in the background\n");
	osmo_timer_schedule(&w->reap_timer, 0, AWORKER_REAP_POLL_MS * 1000);
}

static void restart_timer_cb(void *data)
{
	struct aworker *w = data;

	if (w->pool->stopping)
		return;
	/* worker_apply_policy() only schedules this once the previous process was collected. Check anyway: starting
	 * a second generation next to a live first one would open the same SCTP associations twice. */
	if (w->pid > 0 || w->state == AWORKER_ST_TERMINATING) {
		LOGW(w, LOGL_ERROR, "previous process not collected yet, not starting a new one\n");
		return;
	}
	LOGW(w, LOGL_NOTICE, "restarting worker (start #%u)\n", w->starts + 1);
	if (worker_spawn(w) < 0 && w->state != AWORKER_ST_TERMINATING)
		worker_retry_start(w);
}

/* The worker is gone: drop its link and tell the users right away (that is what confines the failure to this
 * worker's cs7 instances), then collect the process without blocking and apply the failure policy once it is
 * collected. */
static void worker_lost(struct aworker *w, const char *why)
{
	/* Whatever the cause, this process can no longer time out at startup. */
	osmo_timer_del(&w->start_timer);
	/* Already being taken down: the link is gone and the users were told; collection is under way. */
	if (w->state == AWORKER_ST_TERMINATING)
		return;

	/* Stability ends with the loss, not with the collection of the process. */
	w->stable_at_loss = worker_is_stable(w);
	if (w->link) {
		aworker_ipc_link_free(w->link);
		w->link = NULL;
	}
	w->main_fd = -1;
	w->cs7_reported = false;
	memset(&w->cs7, 0, sizeof(w->cs7));
	/* TERMINATING before the users' call-backs run: should one of them lead back here, the check above returns. */
	w->state = w->pid > 0 ? AWORKER_ST_TERMINATING : AWORKER_ST_STOPPED;
	osmo_strlcpy(w->lost_reason, why, sizeof(w->lost_reason));

	/* No SCCP operation is ever replayed: whatever was in flight towards this worker is treated as lost, and
	 * every SCCP connection of its cs7 instances is torn down by the users' 'unavailable' call-back. */
	users_lose_worker(w);

	if (w->pid > 0) {
		w->last_pid = w->pid;
		worker_begin_termination(w);
		return;
	}
	w->last_pid = 0;
	worker_apply_policy(w, false);
}

/***********************************************************************
 * IPC link call-backs
 ***********************************************************************/

static void link_ready_cb(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	struct aworker *w = aworker_ipc_link_get_priv(link);

	LOGW(w, LOGL_INFO, "IPC handshake complete (peer pid %u, ident '%s'); waiting for CS7_INFO\n",
	     peer->pid, peer->ident);
}

/* Check a CS7_INFO against the expected ownership; on a mismatch, log it, record the detail and \returns false. */
static bool ownership_claim_ok(struct aworker *w, const struct aworker_ipc_cs7_info *info)
{
	const struct ownership *own = &w->pool->own;
	bool claimed[AWORKER_POOL_MAX_CS7_ID] = {};
	unsigned int i;
	uint32_t id;

	for (i = 0; i < info->count; i++) {
		int exp;

		id = info->e[i].cs7_id;
		exp = id < AWORKER_POOL_MAX_CS7_ID ? own->owner[id] : -1;
		if (id < AWORKER_POOL_MAX_CS7_ID)
			claimed[id] = true;
		if (exp == (int)w->idx)
			continue;
		if (exp >= 0)
			snprintf(w->fail_detail, sizeof(w->fail_detail), "claims cs7 instance %u, which the config"
				 " file of worker %d configured at start", id, exp);
		else if (own->known[w->idx])
			snprintf(w->fail_detail, sizeof(w->fail_detail), "claims cs7 instance %u, which its config"
				 " file did not configure at start", id);
		else
			continue;	/* nothing expected of this worker; the first-come check below applies */
		goto mismatch;
	}
	if (!own->known[w->idx])
		return true;
	for (id = 0; id < AWORKER_POOL_MAX_CS7_ID; id++) {
		if (own->owner[id] == (int)w->idx && !claimed[id]) {
			snprintf(w->fail_detail, sizeof(w->fail_detail), "does not report cs7 instance %u, which its"
				 " config file configured at start", id);
			goto mismatch;
		}
	}
	return true;

mismatch:
	LOGW(w, LOGL_ERROR, "CS7_INFO does not match the ownership expected from the config files at start: %s"
	     " (was a config file changed?); closing link\n", w->fail_detail);
	return false;
}

static void handle_cs7_info(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_pool *pool = w->pool;
	unsigned int i, j;

	if (w->cs7_reported) {
		LOGW(w, LOGL_ERROR, "second CS7_INFO on the same link; closing link\n");
		aworker_ipc_link_close(w->link);
		return;
	}

	/* The claim must match what the worker config files said at start (see struct ownership): a worker never
	 * takes an instance another worker's file configures, and a worker whose own set is known reports exactly
	 * that set. Anything else means the file was changed after osmo-bsc started; restarting the worker would
	 * read the same file again, so it is FAILED until an operator resets it. */
	if (!ownership_claim_ok(w, &m->u.cs7_info)) {
		w->pending_fail = AWORKER_FAIL_OWNERSHIP;
		aworker_ipc_link_close(w->link);
		return;
	}

	/* A cs7 instance belongs to exactly one worker. A second claim is a configuration error: the same asp /
	 * as config would have been read twice, and two SCTP associations would use the same point code and the
	 * same SCCP local reference space. Refuse the worker rather than route traffic ambiguously. */
	for (i = 0; i < m->u.cs7_info.count; i++) {
		uint32_t id = m->u.cs7_info.e[i].cs7_id;
		for (j = 0; j < pool->num; j++) {
			struct aworker *other = pool->worker[j];
			if (other == w || !other->cs7_reported)
				continue;
			if (cs7_entry(other, id)) {
				LOGW(w, LOGL_FATAL, "claims cs7 instance %u, which worker %u already owns."
				     " Each cs7 instance must be configured in exactly one worker config file;"
				     " closing link\n", id, other->idx);
				aworker_ipc_link_close(w->link);
				return;
			}
		}
	}

	w->cs7 = m->u.cs7_info;
	w->cs7_reported = true;
	w->state = AWORKER_ST_READY;
	osmo_timer_del(&w->start_timer);
	w->ready_seen = true;
	osmo_clock_gettime(CLOCK_MONOTONIC, &w->ready_since);

	for (i = 0; i < w->cs7.count; i++) {
		const struct aworker_ipc_cs7_entry *e = &w->cs7.e[i];
		LOGW(w, LOGL_NOTICE, "owns cs7 instance %u: point-code %u, %s, %u AS(s)\n",
		     e->cs7_id, e->primary_pc, osmo_ss7_asp_protocol_name(e->asp_proto), e->num_as);
	}

	users_try_bind_all(pool);
}

static void handle_bind_result(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_user *au = user_by_ref(w, m->user_ref);

	if (!au) {
		LOGW(w, LOGL_NOTICE, "USER_BIND_RESULT for unknown user_ref %u, ignoring\n", m->user_ref);
		return;
	}
	if (m->u.bind_result != AWORKER_IPC_BIND_OK) {
		LOGU(au, LOGL_ERROR, "worker %u refused USER_BIND: result %u\n", w->idx, m->u.bind_result);
		/* Leave bind_sent set: the worker told us this cs7/ssn combination does not work, so do not spin
		 * on it. It is retried when the worker is restarted. */
		au->bound = false;
		return;
	}
	if (au->bound) {
		LOGU(au, LOGL_NOTICE, "duplicate USER_BIND_RESULT, ignoring\n");
		return;
	}
	au->bound = true;
	LOGU(au, LOGL_NOTICE, "bound in worker %u\n", w->idx);
	if (au->ops && au->ops->available)
		au->ops->available(au->user);
}

static void handle_scu_up(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_user *au = user_by_ref(w, m->user_ref);
	struct osmo_scu_prim *prim;
	struct msgb *upmsg;

	if (!au) {
		/* Can legitimately race an unbind, or be a record of a user we gave up on. */
		LOGW(w, LOGL_NOTICE, "SCU_UP for unknown user_ref %u, dropping\n", m->user_ref);
		return;
	}
	if (!au->bound) {
		LOGU(au, LOGL_NOTICE, "SCU_UP before USER_BIND_RESULT, dropping\n");
		return;
	}
	upmsg = aworker_ipc_build_scu_prim(au->pool->ctx, m);
	if (!upmsg) {
		LOGU(au, LOGL_ERROR, "cannot allocate SCCP primitive, dropping SCU_UP\n");
		return;
	}
	prim = (struct osmo_scu_prim *)msgb_data(upmsg);
	/* Same contract as the in-process backend: the up-call owns the msgb. */
	au->user->prim_cb(au->user, &prim->oph);
}

static void handle_tx_failed(struct aworker *w, const struct aworker_ipc_msg *m)
{
	const struct aworker_ipc_tx_failed *f = &m->u.tx_failed;
	struct aworker_user *au = user_by_ref(w, m->user_ref);

	if (!au) {
		LOGW(w, LOGL_NOTICE, "TX_FAILED for unknown user_ref %u, ignoring\n", m->user_ref);
		return;
	}
	LOGU(au, f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? LOGL_ERROR : LOGL_FATAL,
	     "worker %u could not send %s (seq %u, conn_id %u): %s, code %u, lib_rc %d%s\n",
	     w->idx, aworker_ipc_msg_type_name(f->ref_type), f->ref_seq, f->conn_id,
	     f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? "not executed" : "executed with an error",
	     f->code, f->lib_rc,
	     f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? "" :
	     " -- the operation may or may not have reached the network and is NOT retried");
	if (au->ops && au->ops->tx_failed)
		au->ops->tx_failed(au->user, f);
}

static void link_rx_cb(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	struct aworker *w = aworker_ipc_link_get_priv(link);

	switch (m->type) {
	case AWORKER_IPC_MT_CS7_INFO:
		handle_cs7_info(w, m);
		return;
	case AWORKER_IPC_MT_USER_BIND_RESULT:
		handle_bind_result(w, m);
		return;
	case AWORKER_IPC_MT_SCU_UP:
		handle_scu_up(w, m);
		return;
	case AWORKER_IPC_MT_TX_FAILED:
		handle_tx_failed(w, m);
		return;
	case AWORKER_IPC_MT_GOODBYE:
		LOGW(w, LOGL_NOTICE, "worker says GOODBYE (reason %u)\n", m->u.goodbye_reason);
		aworker_ipc_link_close(link);
		return;
	default:
		/* aworker_ipc_dec() already rejected everything the main process may not receive. */
		LOGW(w, LOGL_ERROR, "unexpected %s from worker; closing link\n",
		     aworker_ipc_msg_type_name(m->type));
		aworker_ipc_link_close(link);
		return;
	}
}

static void link_closed_cb(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	struct aworker *w = aworker_ipc_link_get_priv(link);
	char why[128];

	snprintf(why, sizeof(why), "IPC link closed: %s (%d), last record written %u, last queued %u",
		 aworker_ipc_close_reason_name(info->reason), info->err, info->last_written_seq,
		 info->last_queued_seq);
	worker_lost(w, why);
}

static const struct aworker_ipc_link_ops link_ops = {
	.ready_cb = link_ready_cb,
	.rx_cb = link_rx_cb,
	.closed_cb = link_closed_cb,
};

/***********************************************************************
 * Starting workers
 ***********************************************************************/

static int link_create(struct aworker *w, int fd)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_MAIN,
		.worker_idx = w->idx,
		.pid = (uint32_t)getpid(),
		.ident = "osmo-bsc",
		.log_cat = DAWORKER,
	};
	int rc;

	rc = aworker_ipc_link_alloc(&w->link, w->pool->ctx, fd, &cfg, &link_ops, w);
	if (rc < 0) {
		LOGW(w, LOGL_ERROR, "cannot set up the IPC link: %s\n", strerror(-rc));
		return rc;
	}
	w->main_fd = fd;
	w->state = AWORKER_ST_STARTING;
	w->starts++;
	/* Every start gets the full startup-timeout (re-arming replaces any earlier deadline). */
	osmo_timer_schedule(&w->start_timer, w->pool->cfg.startup_timeout, 0);
	return 0;
}

/* The current process of this worker did not become READY within startup-timeout of its start. The timer is only
 * pending while the worker is STARTING; the state check keeps a callback that should not run from touching any later
 * state or process. The loss goes through the usual path: collection, then the failure policy (backoff, restart
 * limit). Only this worker is affected. */
static void start_timer_cb(void *data)
{
	struct aworker *w = data;

	if (w->state != AWORKER_ST_STARTING)
		return;
	LOGW(w, LOGL_ERROR, "not ready %u s after its start (start #%u); treating the worker as lost\n",
	     w->pool->cfg.startup_timeout, w->starts);
	worker_lost(w, "startup timeout");
}

/* Starting a worker process
 * =========================
 *
 * A worker is fork()ed and then execv()ed: it is a fresh osmo-bsc process that reads only its own configuration
 * file and shares nothing with the main process but the one IPC socket. exec() -- rather than a plain fork() --
 * is what makes a worker restart safe: the main process is fully up by then and holds the Abis, MGW, CTRL and VTY
 * sockets, none of which a worker may inherit.
 */

/* Options of our own command line that a worker should use too (logging only). */
static bool is_log_opt(const char *arg, bool *takes_value)
{
	static const char *const flags[] = { "-s", "--disable-color", "-T", "--timestamp", NULL };
	static const char *const with_value[] = { "-d", "--debug", "-e", "--log-level", NULL };
	unsigned int i;

	*takes_value = false;
	for (i = 0; flags[i]; i++) {
		if (!strcmp(arg, flags[i]))
			return true;
	}
	for (i = 0; with_value[i]; i++) {
		size_t l = strlen(with_value[i]);
		if (!strcmp(arg, with_value[i])) {
			*takes_value = true;
			return true;
		}
		/* --debug=DRLL:DMM form */
		if (with_value[i][1] == '-' && !strncmp(arg, with_value[i], l) && arg[l] == '=')
			return true;
	}
	return false;
}

/* Move a descriptor that landed on 0, 1 or 2 (because osmo-bsc was started with a standard stream closed) above
 * them, so that it can never be mistaken for, or replaced as, a standard stream. \returns the new fd or -1. */
static int fd_above_stdio(int fd, bool cloexec)
{
	int nfd;

	if (fd < 0 || fd > 2)
		return fd;
	nfd = fcntl(fd, cloexec ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
	close(fd);
	return nfd;
}

void aworker_pool_set_exec(const char *argv0, int argc, char **argv)
{
	char path[PATH_MAX];
	ssize_t len;
	int i;

	talloc_free(g_exec_path);
	g_exec_log_argv = NULL;
	g_exec_log_argc = 0;

	/* /proc/self/exe is immune to a changed working directory and to a bare program name in argv[0]. */
	len = readlink("/proc/self/exe", path, sizeof(path) - 1);
	if (len > 0) {
		path[len] = '\0';
		g_exec_path = talloc_strdup(NULL, path);
	} else {
		g_exec_path = talloc_strdup(NULL, argv0 ? argv0 : "osmo-bsc");
	}
	OSMO_ASSERT(g_exec_path);

	/* Hold on to the binary image itself, not to its name: replacing or deleting the file later (a reinstall
	 * while osmo-bsc runs) must neither break a worker restart nor start a different build next to this one. */
	if (g_exec_fd >= 0)
		close(g_exec_fd);
	g_exec_fd = fd_above_stdio(open("/proc/self/exe", O_RDONLY | O_CLOEXEC), true);
	if (g_exec_fd < 0)
		LOGP(DAWORKER, LOGL_NOTICE, "cannot open /proc/self/exe (%s): workers will be started from the path '%s',"
		     " so replacing that file while osmo-bsc runs would make a restarted worker a different binary\n",
		     strerror(errno), g_exec_path);

	g_exec_log_argv = talloc_zero_array(g_exec_path, char *, argc + 1);
	OSMO_ASSERT(g_exec_log_argv);
	for (i = 1; i < argc; i++) {
		bool takes_value;
		if (!is_log_opt(argv[i], &takes_value))
			continue;
		g_exec_log_argv[g_exec_log_argc++] = talloc_strdup(g_exec_log_argv, argv[i]);
		if (takes_value && i + 1 < argc)
			g_exec_log_argv[g_exec_log_argc++] = talloc_strdup(g_exec_log_argv, argv[++i]);
	}
}

/* Descriptor hygiene in the child, between fork() and exec(): close EVERY descriptor except stdio and the ones in
 * keep[] (sorted ascending, all > 2). No fixed limit: close_range() if the kernel has it, otherwise /proc/self/fd
 * is swept until nothing is left to close, and only without /proc the RLIMIT_NOFILE range is closed. */
static bool is_kept(int fd, const int *keep, unsigned int n_keep)
{
	unsigned int i;

	for (i = 0; i < n_keep; i++) {
		if (keep[i] == fd)
			return true;
	}
	return false;
}

static int sys_close_range(unsigned int first, unsigned int last)
{
#ifdef SYS_close_range
	return (int)syscall(SYS_close_range, first, last, 0);
#else
	errno = ENOSYS;
	return -1;
#endif
}

static void close_fds_by_rlimit(const int *keep, unsigned int n_keep)
{
	struct rlimit rl;
	unsigned long max = 1024, fd;

	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
		max = OSMO_MAX((unsigned long)rl.rlim_cur, 1024UL);
	else
		max = 1UL << 20;
	for (fd = 3; fd < max; fd++) {
		if (!is_kept((int)fd, keep, n_keep))
			close((int)fd);
	}
}

static void close_fds_by_proc(const int *keep, unsigned int n_keep)
{
	for (;;) {
		int batch[128];
		unsigned int n = 0, i;
		struct dirent *de;
		DIR *d = opendir("/proc/self/fd");

		if (!d) {
			/* Not even /proc: we cannot see which descriptors are open, so close the whole range. */
			static const char msg[] = "osmo-bsc: aworker: no close_range() and no /proc/self/fd; closing"
						  " descriptors up to RLIMIT_NOFILE\n";
			if (write(2, msg, sizeof(msg) - 1) < 0) {}
			close_fds_by_rlimit(keep, n_keep);
			return;
		}
		while ((de = readdir(d)) && n < ARRAY_SIZE(batch)) {
			char *end;
			long fd = strtol(de->d_name, &end, 10);
			if (*end || end == de->d_name || fd <= 2 || fd == dirfd(d) || is_kept((int)fd, keep, n_keep))
				continue;
			batch[n++] = (int)fd;
		}
		closedir(d);
		if (!n)
			return;		/* a full pass found nothing more to close */
		for (i = 0; i < n; i++)
			close(batch[i]);
	}
}

static void child_close_fds(const int *keep, unsigned int n_keep)
{
	unsigned int first = 3, i;

	for (i = 0; i <= n_keep; i++) {
		unsigned int last = i < n_keep ? (unsigned int)keep[i] - 1 : ~0U;
		if (first <= last && sys_close_range(first, last) < 0) {
			close_fds_by_proc(keep, n_keep);	/* ENOSYS (old kernel), EPERM (seccomp), ... */
			return;
		}
		if (i < n_keep)
			first = (unsigned int)keep[i] + 1;
	}
}

/* Run in the child after fork(); never returns. \param parent pid of the main process, taken before fork(). */
static void child_exec_worker(unsigned int idx, const char *cfg_file, int fd, pid_t parent)
{
	char **argv;
	char fdstr[16], idxstr[16];
	unsigned int n = 0, i;
	int keep[2], n_keep = 0;
	int flags;

	/* Die with the main process, whatever happens to it -- including SIGKILL, which leaves no chance to
	 * send GOODBYE. This survives the execv() below. */
	prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
	/* ... but only for a parent death AFTER this call. If the main process is already gone, we were re-parented
	 * and nobody would ever tell us: do not become an orphan worker with an SCTP association of its own. */
	if (getppid() != parent)
		_exit(AWORKER_EXIT_LINK);

	/* The IPC socket must survive exec(), and it must not sit on a standard stream's number. */
	if (fd <= 2) {
		int old = fd, null_fd;
		fd = fcntl(old, F_DUPFD, 3);
		if (fd < 0)
			_exit(AWORKER_EXIT_LINK);
		null_fd = open("/dev/null", O_RDWR);
		if (null_fd >= 0 && null_fd != old) {
			dup2(null_fd, old);
			close(null_fd);
		}
	}
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);

	/* Keep the IPC socket, and the executable we exec from (it is O_CLOEXEC, so the worker does not inherit it). */
	if (g_exec_fd >= 0 && g_exec_fd < fd)
		keep[n_keep++] = g_exec_fd;
	keep[n_keep++] = fd;
	if (g_exec_fd >= 0 && g_exec_fd > fd)
		keep[n_keep++] = g_exec_fd;
	child_close_fds(keep, n_keep);

	snprintf(fdstr, sizeof(fdstr), "%d", fd);
	snprintf(idxstr, sizeof(idxstr), "%u", idx);

	argv = calloc(g_exec_log_argc + 8, sizeof(*argv));
	if (!argv)
		_exit(AWORKER_EXIT_CONFIG);
	argv[n++] = g_exec_path;
	for (i = 0; i < g_exec_log_argc; i++)
		argv[n++] = g_exec_log_argv[i];
	argv[n++] = (char *)"--aworker-fd";
	argv[n++] = fdstr;
	argv[n++] = (char *)"--aworker-index";
	argv[n++] = idxstr;
	argv[n++] = (char *)"--aworker-config";
	argv[n++] = (char *)cfg_file;
	argv[n] = NULL;

	/* Same command line and environment as execv() would use, but from the binary image we were started from. */
	if (g_exec_fd >= 0)
		fexecve(g_exec_fd, argv, environ);
	else
		execv(g_exec_path, argv);
	/* exec only returns on failure, and we have no usable logging left here. */
	fprintf(stderr, "osmo-bsc: aworker %u: cannot execute '%s'%s: %s\n", idx, g_exec_path,
		g_exec_fd >= 0 ? " (from the descriptor opened at startup)" : "", strerror(errno));
	_exit(AWORKER_EXIT_CONFIG);
}

static int worker_spawn(struct aworker *w)
{
	struct aworker_pool *pool = w->pool;
	const char *cfg_file = pool->cfg.cfg_file[w->idx];
	pid_t parent = getpid();
	int sp[2];
	pid_t pid;
	int rc;

	OSMO_ASSERT(!pool->no_fork);
	OSMO_ASSERT(cfg_file);
	OSMO_ASSERT(g_exec_path);

	/* Never two generations of one worker: the previous process must have been collected. */
	if (w->pid > 0 || w->state == AWORKER_ST_TERMINATING) {
		LOGW(w, LOGL_ERROR, "refusing to start a new process: the previous one is not collected yet\n");
		return -EBUSY;
	}
	/* A new process has to earn its stability from READY on. */
	w->ready_seen = false;
	w->stable_at_loss = false;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) < 0) {
		rc = -errno;
		LOGP(DAWORKER, LOGL_ERROR, "aworker %u: socketpair() failed: %s\n", w->idx, strerror(-rc));
		return rc;
	}

	pid = fork();
	if (pid < 0) {
		rc = -errno;
		LOGP(DAWORKER, LOGL_ERROR, "aworker %u: fork() failed: %s\n", w->idx, strerror(-rc));
		close(sp[0]);
		close(sp[1]);
		return rc;
	}

	if (pid == 0) {
		close(sp[0]);
		child_exec_worker(w->idx, cfg_file, sp[1], parent);
		/* not reached */
	}

	close(sp[1]);
	w->pid = pid;
	rc = link_create(w, sp[0]);
	if (rc < 0) {
		close(sp[0]);
		/* The process exists: take it down like any lost worker, without blocking; the failure policy
		 * runs once it is collected. */
		osmo_strlcpy(w->lost_reason, "cannot set up the IPC link", sizeof(w->lost_reason));
		w->last_pid = pid;
		worker_begin_termination(w);
		return rc;
	}
	LOGW(w, LOGL_NOTICE, "started '%s'%s with config file '%s'\n", g_exec_path,
	     g_exec_fd >= 0 ? " (from the descriptor opened at startup)" : "", cfg_file);
	return 0;
}

static void startup_timer_cb(void *data)
{
	struct aworker_pool *pool = data;
	struct aworker_user *au;

	/* Only a diagnostic, once, startup-timeout after the pool started. The workers themselves time out per start
	 * (start_timer_cb()). */
	llist_for_each_entry(au, &pool->users, entry) {
		if (au->bound)
			continue;
		LOGU(au, LOGL_ERROR, "still not bound %u s after start: no worker owns cs7 instance %u."
		     " Check that exactly one worker config file configures an 'asp' on it.\n",
		     pool->cfg.startup_timeout, au->cs7_id);
	}
}

static struct aworker_pool *pool_alloc(void *ctx, unsigned int num)
{
	struct aworker_pool *pool;
	unsigned int i;

	pool = talloc_zero(ctx, struct aworker_pool);
	if (!pool)
		return NULL;
	pool->ctx = pool;
	/* Deep copy: the strings belong to the pool. The VTY replaces or frees the ones in g_cfg whenever it is told
	 * to, and the running pool -- worker restarts, 'show a-worker-pool' -- must never see that. */
	pool->cfg = g_cfg;
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		pool->cfg.cfg_file[i] = NULL;
		if (!g_cfg.cfg_file[i])
			continue;
		pool->cfg.cfg_file[i] = talloc_strdup(pool, g_cfg.cfg_file[i]);
		if (!pool->cfg.cfg_file[i]) {
			talloc_free(pool);
			return NULL;
		}
	}
	pool->num = num;
	pool->next_user_ref = 1;
	INIT_LLIST_HEAD(&pool->users);
	osmo_timer_setup(&pool->startup_timer, startup_timer_cb, pool);

	for (i = 0; i < num; i++) {
		struct aworker *w = talloc_zero(pool, struct aworker);
		if (!w) {
			talloc_free(pool);
			return NULL;
		}
		w->pool = pool;
		w->idx = i;
		w->main_fd = -1;
		w->state = AWORKER_ST_STOPPED;
		osmo_timer_setup(&w->restart_timer, restart_timer_cb, w);
		osmo_timer_setup(&w->start_timer, start_timer_cb, w);
		osmo_timer_setup(&w->reap_timer, reap_timer_cb, w);
		pool->worker[i] = w;
	}
	return pool;
}

static void ownership_log(const struct aworker_pool *pool)
{
	unsigned int id;

	for (id = 0; id < AWORKER_POOL_MAX_CS7_ID; id++) {
		int w = pool->own.owner[id];
		if (w >= 0)
			LOGP(DAWORKER, LOGL_INFO, "cs7 instance %u is expected to be owned by worker %d ('%s', line"
			     " %u)\n", id, w, pool->cfg.cfg_file[w], pool->own.lineno[id]);
	}
}

int aworker_pool_expected_owner(uint32_t cs7_id)
{
	if (!g_pool || cs7_id >= AWORKER_POOL_MAX_CS7_ID)
		return -1;
	return g_pool->own.owner[cs7_id];
}

int aworker_pool_start(void *ctx)
{
	struct aworker_pool *pool;
	unsigned int i;
	int rc;

	OSMO_ASSERT(!g_pool);
	if (!aworker_pool_enabled())
		return 0;

	for (i = 0; i < g_cfg.num_workers; i++) {
		if (!g_cfg.cfg_file[i]) {
			LOGP(DAWORKER, LOGL_FATAL,
			     "'a-worker-pool': 'workers %u' is configured but worker %u has no config file."
			     " Add 'worker %u config-file FILE'.\n", g_cfg.num_workers, i, i);
			return -EINVAL;
		}
	}

	pool = pool_alloc(ctx, g_cfg.num_workers);
	if (!pool)
		return -ENOMEM;
	if (ownership_build(&pool->cfg, &pool->own) < 0) {
		talloc_free(pool);
		return -EINVAL;
	}
	ownership_log(pool);
	g_pool = pool;
	/* Covers the exit() calls of a startup that fails after this point: without this, the workers would
	 * keep their SCTP associations open with no osmo-bsc behind them. */
	atexit(aworker_pool_stop);

	for (i = 0; i < pool->num; i++) {
		rc = worker_spawn(pool->worker[i]);
		if (rc < 0) {
			aworker_pool_stop();
			return rc;
		}
	}

	LOGP(DAWORKER, LOGL_NOTICE, "A-worker pool started: %u worker(s), on-worker-failure %s,"
	     " startup-timeout %u s, restart-delay %u s (max %u s), stable-time %u s, max-restarts %u window %u s\n",
	     pool->num, get_value_string(aworker_failure_policy_names, pool->cfg.on_failure),
	     pool->cfg.startup_timeout, pool->cfg.restart_delay,
	     OSMO_MAX(pool->cfg.restart_delay, pool->cfg.restart_delay_max), pool->cfg.stable_time,
	     pool->cfg.max_restarts, pool->cfg.restart_window);
	osmo_timer_schedule(&pool->startup_timer, pool->cfg.startup_timeout, 0);
	return 0;
}

int aworker_pool_start_with_fds(void *ctx, const int *fds, unsigned int num)
{
	struct aworker_pool *pool;
	unsigned int i;
	int rc;

	OSMO_ASSERT(!g_pool);
	if (!num || num > AWORKER_POOL_MAX_WORKERS)
		return -EINVAL;

	pool = pool_alloc(ctx, num);
	if (!pool)
		return -ENOMEM;
	if (ownership_build(&pool->cfg, &pool->own) < 0) {
		talloc_free(pool);
		return -EINVAL;
	}
	pool->no_fork = true;
	g_pool = pool;

	for (i = 0; i < num; i++) {
		rc = link_create(pool->worker[i], fds[i]);
		if (rc < 0) {
			aworker_pool_stop();
			return rc;
		}
	}
	osmo_timer_schedule(&pool->startup_timer, pool->cfg.startup_timeout, 0);
	return 0;
}

/* aworker_pool_stop() only: queue GOODBYE on \a w's link, unless done before. \returns true if it is queued. May
 * lose the worker (a link that cannot take the record closes), so the caller must re-read w->link. */
static bool queue_goodbye(struct aworker_pool *pool, struct aworker *w)
{
	struct msgb *rec;

	if (w->goodbye_queued)
		return true;
	if (!w->link || aworker_ipc_link_get_state(w->link) != AWORKER_IPC_LINK_READY)
		return false;
	if (aworker_ipc_enc_goodbye(pool->ctx, &rec, AWORKER_IPC_GOODBYE_SHUTDOWN) < 0)
		return false;
	if (aworker_ipc_link_send(w->link, rec, &w->goodbye_seq) < 0)
		return false;
	w->goodbye_queued = true;
	LOGW(w, LOGL_INFO, "GOODBYE queued (seq %u)\n", w->goodbye_seq);
	return true;
}

/* Has \a w's GOODBYE left this process? Records it in w->goodbye_written. */
static bool goodbye_written(struct aworker *w)
{
	struct aworker_ipc_link_stats st;

	if (!w->goodbye_written && w->goodbye_queued && w->link) {
		aworker_ipc_link_get_stats(w->link, &st);
		w->goodbye_written = st.last_written_seq >= w->goodbye_seq;
	}
	return w->goodbye_written;
}

/* A GOODBYE is queued but not written yet, on a link that is still open. */
static bool goodbye_pending(const struct aworker_pool *pool)
{
	unsigned int i;

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->link && w->goodbye_queued && !goodbye_written(w))
			return true;
	}
	return false;
}

/* aworker_pool_stop() only: collect whatever has exited, until all are collected or \a ms have passed. Runs after
 * the event loop has ended (osmo-bsc is exiting), so sleeping here blocks nothing; the wait is bounded.
 * \returns true if no process is left. */
static bool stop_wait(struct aworker_pool *pool, unsigned int ms)
{
	struct timespec t0;
	unsigned int i;

	osmo_clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;;) {
		bool left = false;
		for (i = 0; i < pool->num; i++) {
			struct aworker *w = pool->worker[i];
			int status;
			pid_t rc;

			if (w->pid <= 0)
				continue;
			rc = waitpid(w->pid, &status, WNOHANG);
			if (rc == w->pid) {
				log_exit_status(w, status);
				w->pid = 0;
			} else if (rc < 0 && errno == ECHILD) {
				w->pid = 0;	/* already reaped elsewhere; never signal that pid again */
			} else {
				left = true;
			}
		}
		if (!left)
			return true;
		if (ms_since(&t0) >= ms)
			return false;
		usleep(STOP_POLL_MS * 1000);
	}
}

/* Take all processes down together (not one after the other): SIGTERM to all, wait up to the grace period, SIGKILL
 * to the rest, wait a bounded time again. A process that is still not collected after that is reported, and not
 * pretended gone; having received SIGKILL, it cannot run any of our code any more. */
static void stop_processes(struct aworker_pool *pool)
{
	unsigned int i;

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->pid > 0 && !w->sigkill_sent)
			kill(w->pid, SIGTERM);
	}
	if (stop_wait(pool, AWORKER_TERM_GRACE_MS))
		return;
	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->pid <= 0)
			continue;
		LOGW(w, LOGL_ERROR, "did not exit after SIGTERM, sending SIGKILL\n");
		kill(w->pid, SIGKILL);
	}
	if (stop_wait(pool, STOP_KILL_WAIT_MS))
		return;
	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->pid > 0)
			LOGW(w, LOGL_FATAL, "still not collected %u ms after SIGKILL (stuck in the kernel?); leaving it"
			     " to the kernel\n", STOP_KILL_WAIT_MS);
	}
}

void aworker_pool_stop(void)
{
	struct aworker_pool *pool = g_pool;
	struct aworker_user *au;
	bool goodbye_delivered = false;
	unsigned int i;

	if (!pool)
		return;
	/* No worker is (re)started from here on: neither the failure policy (see 'stopping') nor a timer that
	 * fires while the event loop is polled below. */
	pool->stopping = true;
	osmo_timer_del(&pool->startup_timer);
	for (i = 0; i < pool->num; i++) {
		osmo_timer_del(&pool->worker[i]->restart_timer);
		osmo_timer_del(&pool->worker[i]->start_timer);
	}

	/* Tell every worker GOODBYE, so that it exits by itself (status 0). Queueing is not enough: osmo-bsc's
	 * event loop has ended, and freeing a link drops whatever it has not written yet. So keep polling the
	 * event loop, without blocking and for a bounded time, until every GOODBYE is written. (Queueing it from
	 * the main loop once a shutdown is requested would not do either: a request that arrives while the loop
	 * dispatches ends the loop in that same iteration, as libosmocore only counts the writes pending then.)
	 * A link that closes meanwhile loses its worker the usual way. */
	for (i = 0; i < pool->num; i++)
		queue_goodbye(pool, pool->worker[i]);
	if (goodbye_pending(pool)) {
		struct timespec t0;

		osmo_clock_gettime(CLOCK_MONOTONIC, &t0);
		while (goodbye_pending(pool) && ms_since(&t0) < STOP_FLUSH_MS) {
			if (!osmo_select_main_ctx(1))
				usleep(1000);
		}
	}

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];

		osmo_timer_del(&w->reap_timer);
		if (goodbye_written(w) && w->pid > 0)
			goodbye_delivered = true;
		if (w->link) {
			aworker_ipc_link_free(w->link);
			w->link = NULL;
		}
		w->main_fd = -1;
		w->state = AWORKER_ST_STOPPED;
	}
	/* A worker that got GOODBYE exits 0 by itself: give it the grace period before anyone is signalled. */
	if (goodbye_delivered)
		stop_wait(pool, AWORKER_TERM_GRACE_MS);
	stop_processes(pool);

	/* The users outlive the pool (they are owned by the A interface code), so detach them explicitly. */
	while (!llist_empty(&pool->users)) {
		au = llist_first_entry(&pool->users, struct aworker_user, entry);
		llist_del(&au->entry);
		au->pool = NULL;
		au->worker = NULL;
		au->bind_sent = false;
		au->bound = false;
	}

	g_pool = NULL;
	talloc_free(pool);
}

bool aworker_pool_shutdown_was_fatal(void)
{
	return g_fatal;
}

unsigned int aworker_pool_num_workers(void)
{
	return g_pool ? g_pool->num : 0;
}

int aworker_pool_worker_info(unsigned int idx, struct aworker_pool_worker_info *out)
{
	const struct aworker *w;

	if (!g_pool)
		return -ENODEV;
	if (idx >= g_pool->num || !out)
		return -EINVAL;
	w = g_pool->worker[idx];
	*out = (struct aworker_pool_worker_info){
		.idx = w->idx,
		.state = w->state,
		.pid = w->pid,
		.starts = w->starts,
		.cfg_file = g_pool->cfg.cfg_file[idx],
		.sigkill_sent = w->sigkill_sent,
		.reap_stuck = w->reap_stuck,
		.fail_reason = w->fail_reason,
		.fail_detail = w->fail_detail,
		.failed_at = w->failed_at,
		.backoff_step = worker_next_step(w),
		.restart_delay = w->restart_delay_s,
		.restarts_in_window = restarts_in_window(w),
	};
	return 0;
}

int aworker_pool_reset_worker(unsigned int idx)
{
	struct aworker *w;

	if (!g_pool)
		return -ENODEV;
	if (idx >= g_pool->num)
		return -EINVAL;
	if (g_pool->stopping)
		return -ESHUTDOWN;
	w = g_pool->worker[idx];
	if (w->state != AWORKER_ST_FAILED)
		return -EBUSY;

	LOGW(w, LOGL_NOTICE, "reset by the operator (was FAILED: %s: %s); starting it again\n",
	     aworker_fail_reason_name(w->fail_reason), w->fail_detail);
	w->fail_reason = AWORKER_FAIL_NONE;
	w->fail_detail[0] = '\0';
	w->failed_at = 0;
	/* A fresh start: no backoff carried over from before the failure. */
	w->backoff_step = 0;
	w->restart_delay_s = g_pool->cfg.restart_delay;
	w->num_restart_at = 0;
	w->state = AWORKER_ST_STOPPED;
	if (g_pool->no_fork)
		return 0;	/* no process of our own to start */
	if (worker_spawn(w) < 0 && w->state != AWORKER_ST_TERMINATING)
		worker_retry_start(w);
	return 0;
}

bool aworker_pool_all_ready(void)
{
	unsigned int i;

	if (!g_pool)
		return false;
	for (i = 0; i < g_pool->num; i++) {
		if (g_pool->worker[i]->state != AWORKER_ST_READY)
			return false;
	}
	return true;
}

bool aworker_pool_cs7_info(uint32_t cs7_id, struct aworker_ipc_cs7_entry *out)
{
	const struct aworker_ipc_cs7_entry *e;
	struct aworker *w;

	if (!g_pool)
		return false;
	w = worker_by_cs7(g_pool, cs7_id);
	if (!w)
		return false;
	e = cs7_entry(w, cs7_id);
	if (!e)
		return false;
	*out = *e;
	return true;
}

/***********************************************************************
 * SCCP User backend: main process -> worker
 ***********************************************************************/

/* Common tail of all transmit operations: hand the record to the link. */
static int user_send(struct aworker_user *au, struct msgb *rec, uint8_t type, uint32_t conn_id)
{
	uint32_t seq = 0;
	int rc;

	rc = aworker_ipc_link_send(au->worker->link, rec, &seq);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot send %s (conn_id %u) to worker %u: %s\n",
		     aworker_ipc_msg_type_name(type), conn_id, au->worker->idx, strerror(-rc));
		return rc;
	}
	LOGU(au, LOGL_DEBUG, "sent %s (conn_id %u, seq %u) to worker %u\n", aworker_ipc_msg_type_name(type),
	     conn_id, seq, au->worker->idx);
	return 0;
}

/* NULL if this user cannot currently transmit. */
static struct aworker_user *tx_user(struct bsc_sccp_user *user)
{
	struct aworker_user *au = user->worker_priv;

	if (!au || !au->bound || !au->worker || !au->worker->link)
		return NULL;
	if (aworker_ipc_link_get_state(au->worker->link) != AWORKER_IPC_LINK_READY)
		return NULL;
	return au;
}

/* The SCCP operations are bounded by what the pinned libosmo-sigtran transmit path can encode; see
 * aworker_ipc_sccp_max_data(). Checking here keeps the error synchronous (the caller sees -EMSGSIZE and frees
 * nothing twice); the worker checks again before it calls the library. */
static int check_len(struct aworker_user *au, uint8_t type, unsigned int len)
{
	int rc = aworker_ipc_sccp_check_data(type, len);

	if (rc < 0)
		LOGU(au, LOGL_ERROR, "%s: %u octets of user data exceed the %d the A interface supports\n",
		     aworker_ipc_msg_type_name(type), len, aworker_ipc_sccp_max_data(type));
	return rc;
}

static int w_tx_unitdata_msg(struct bsc_sccp_user *user,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_UNITDATA, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	/* The encoder always takes ownership of msg, like osmo_sccp_tx_unitdata_msg(). */
	rc = aworker_ipc_enc_tx_unitdata(au->pool->ctx, &rec, au->user_ref, calling_addr, called_addr, msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_UNITDATA: %s\n", strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_UNITDATA, AWORKER_IPC_CONN_ID_NONE);
}

static int w_tx_conn_req_msg(struct bsc_sccp_user *user, uint32_t conn_id,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_CONN_REQ, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	rc = aworker_ipc_enc_tx_conn_req(au->pool->ctx, &rec, au->user_ref, conn_id, calling_addr, called_addr,
					 msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_CONN_REQ (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_CONN_REQ, conn_id);
}

static int w_tx_conn_resp(struct bsc_sccp_user *user, uint32_t conn_id,
			  const struct osmo_sccp_addr *resp_addr,
			  const uint8_t *data, unsigned int len)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au)
		return -ENOTCONN;
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_CONN_RESP, len)) < 0)
		return rc;
	rc = aworker_ipc_enc_tx_conn_resp(au->pool->ctx, &rec, au->user_ref, conn_id, resp_addr, data, len);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_CONN_RESP (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_CONN_RESP, conn_id);
}

static int w_tx_data_msg(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_DATA, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	rc = aworker_ipc_enc_tx_data(au->pool->ctx, &rec, au->user_ref, conn_id, msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_DATA (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_DATA, conn_id);
}

static int w_tx_disconn(struct bsc_sccp_user *user, uint32_t conn_id,
			const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au)
		return -ENOTCONN;
	rc = aworker_ipc_enc_tx_disconn(au->pool->ctx, &rec, au->user_ref, conn_id, resp_addr, cause);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_DISCONN (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_DISCONN, conn_id);
}

const struct bsc_sccp_backend_ops bsc_sccp_backend_worker_ops = {
	.name = "worker",
	.tx_unitdata_msg = w_tx_unitdata_msg,
	.tx_conn_req_msg = w_tx_conn_req_msg,
	.tx_conn_resp = w_tx_conn_resp,
	.tx_data_msg = w_tx_data_msg,
	.tx_disconn = w_tx_disconn,
};

/***********************************************************************
 * SCCP User binding
 ***********************************************************************/

struct bsc_sccp_user *aworker_sccp_user_bind(void *ctx, struct osmo_ss7_instance *ss7,
					     struct bsc_sccp_inst *bsc_sccp, const char *name,
					     bsc_sccp_user_prim_cb prim_cb, uint16_t ssn,
					     const struct aworker_sccp_user_ops *ops)
{
	struct bsc_sccp_user *user;
	struct aworker_user *au;
	uint32_t cs7_id;

	OSMO_ASSERT(ss7);
	OSMO_ASSERT(bsc_sccp);
	OSMO_ASSERT(prim_cb);
	OSMO_ASSERT(name && *name && strlen(name) <= AWORKER_IPC_MAX_USER_NAME);

	if (!g_pool) {
		LOGP(DAWORKER, LOGL_ERROR, "cannot bind SCCP user '%s': the A-worker pool is not running\n", name);
		return NULL;
	}
	cs7_id = osmo_ss7_instance_get_id(ss7);
	if (user_by_cs7_ssn(g_pool, cs7_id, ssn)) {
		LOGP(DAWORKER, LOGL_ERROR, "cannot bind SCCP user '%s': cs7 instance %u SSN %u is already bound\n",
		     name, cs7_id, ssn);
		return NULL;
	}

	user = talloc_zero(ctx, struct bsc_sccp_user);
	if (!user)
		return NULL;
	au = talloc_zero(user, struct aworker_user);
	if (!au) {
		talloc_free(user);
		return NULL;
	}

	au->pool = g_pool;
	au->cs7_id = cs7_id;
	au->ssn = ssn;
	OSMO_STRLCPY_ARRAY(au->name, name);
	au->ops = ops;
	au->user = user;

	*user = (struct bsc_sccp_user){
		.ops = &bsc_sccp_backend_worker_ops,
		.prim_cb = prim_cb,
		.ss7 = ss7,
		.bsc_sccp = bsc_sccp,
		.worker_priv = au,
	};

	llist_add_tail(&au->entry, &g_pool->users);
	LOGU(au, LOGL_NOTICE, "SCCP user registered for the A-worker pool\n");
	user_try_bind(au);
	return user;
}

struct bsc_sccp_user *aworker_sccp_user_find(uint32_t cs7_id, uint16_t ssn)
{
	struct aworker_user *au;

	if (!g_pool)
		return NULL;
	au = user_by_cs7_ssn(g_pool, cs7_id, ssn);
	return au ? au->user : NULL;
}

void aworker_sccp_user_unbind(struct bsc_sccp_user *user)
{
	struct aworker_user *au;

	if (!user)
		return;
	au = user->worker_priv;
	if (au) {
		if (au->bind_sent && au->worker && au->worker->link) {
			struct msgb *rec;
			if (aworker_ipc_enc_user_unbind(au->pool->ctx, &rec, au->user_ref) == 0)
				aworker_ipc_link_send(au->worker->link, rec, NULL);
		}
		if (au->pool)
			llist_del(&au->entry);
	}
	talloc_free(user);
}

bool aworker_sccp_user_ready(const struct bsc_sccp_user *user)
{
	const struct aworker_user *au;

	if (!user || user->ops != &bsc_sccp_backend_worker_ops)
		return false;
	au = user->worker_priv;
	return au && au->bound;
}

/***********************************************************************
 * Diagnostics
 ***********************************************************************/

void aworker_pool_dump_vty(struct vty *vty)
{
	struct aworker_user *au;
	unsigned int i, j;

	if (!aworker_pool_enabled()) {
		vty_out(vty, "A-worker pool: disabled (the SIGTRAN stack runs in the osmo-bsc process)%s",
			VTY_NEWLINE);
		return;
	}
	if (!g_pool) {
		vty_out(vty, "A-worker pool: configured for %u worker(s), not started%s", g_cfg.num_workers,
			VTY_NEWLINE);
		return;
	}

	vty_out(vty, "A-worker pool: %u worker(s), on-worker-failure %s, startup-timeout %u s,"
		" restart-delay %u s (max %u s), stable-time %u s%s", g_pool->num,
		get_value_string(aworker_failure_policy_names, g_pool->cfg.on_failure), g_pool->cfg.startup_timeout,
		g_pool->cfg.restart_delay, OSMO_MAX(g_pool->cfg.restart_delay, g_pool->cfg.restart_delay_max),
		g_pool->cfg.stable_time, VTY_NEWLINE);
	vty_out(vty, " max-restarts %u window %u s: a loss that would be restart %u within %u s makes a worker"
		" FAILED (at most %u restarts)%s", g_pool->cfg.max_restarts, g_pool->cfg.restart_window,
		g_pool->cfg.max_restarts, g_pool->cfg.restart_window, g_pool->cfg.max_restarts - 1, VTY_NEWLINE);

	for (i = 0; i < g_pool->num; i++) {
		struct aworker *w = g_pool->worker[i];
		struct aworker_ipc_link_stats st;

		vty_out(vty, " worker %u: state %s, pid %ld, starts %u, config file %s%s", w->idx,
			get_value_string(state_names, w->state), (long)w->pid, w->starts,
			g_pool->cfg.cfg_file[i] ? g_pool->cfg.cfg_file[i] : "(none)", VTY_NEWLINE);
		if (w->state == AWORKER_ST_FAILED) {
			char when[32];
			struct tm tm;
			strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", localtime_r(&w->failed_at, &tm));
			vty_out(vty, "  failed: %s, at %s: %s%s", aworker_fail_reason_name(w->fail_reason), when,
				w->fail_detail, VTY_NEWLINE);
			vty_out(vty, "  not restarted; 'a-worker-pool reset worker %u' starts it again%s", w->idx,
				VTY_NEWLINE);
		}
		if (w->state == AWORKER_ST_RESTARTING)
			vty_out(vty, "  restarting after %u s (unplanned restart %u in a row)%s", w->restart_delay_s,
				w->backoff_step, VTY_NEWLINE);
		else if (w->state != AWORKER_ST_FAILED && worker_next_step(w))
			vty_out(vty, "  unplanned restarts in a row: %u; if lost now, restarted after %u s"
				" (back to %u s once READY for %u s)%s", w->backoff_step,
				restart_delay_for(&g_pool->cfg, w->backoff_step), g_pool->cfg.restart_delay,
				g_pool->cfg.stable_time, VTY_NEWLINE);
		if (w->state != AWORKER_ST_FAILED && restarts_in_window(w))
			vty_out(vty, "  unplanned restarts within the last %u s: %u (restart %u would make it FAILED)%s",
				g_pool->cfg.restart_window, restarts_in_window(w), g_pool->cfg.max_restarts, VTY_NEWLINE);
		if (w->state == AWORKER_ST_TERMINATING)
			vty_out(vty, "  terminating: %s%s",
				w->reap_stuck ? "SIGKILL sent, process still not collected (stuck in the kernel?)"
				: w->sigkill_sent ? "SIGKILL sent, waiting to collect the process"
				: "SIGTERM sent, waiting to collect the process", VTY_NEWLINE);
		if (g_cfg.cfg_file[i] && g_pool->cfg.cfg_file[i] && strcmp(g_cfg.cfg_file[i], g_pool->cfg.cfg_file[i]))
			vty_out(vty, "  configured config file %s takes effect after restarting osmo-bsc%s",
				g_cfg.cfg_file[i], VTY_NEWLINE);
		for (j = 0; j < w->cs7.count; j++) {
			const struct aworker_ipc_cs7_entry *e = &w->cs7.e[j];
			vty_out(vty, "  cs7 instance %u: point-code %u, %s, %u AS(s)%s", e->cs7_id,
				e->primary_pc, osmo_ss7_asp_protocol_name(e->asp_proto), e->num_as, VTY_NEWLINE);
		}
		if (!w->link) {
			vty_out(vty, "  IPC link: down%s", VTY_NEWLINE);
			continue;
		}
		aworker_ipc_link_get_stats(w->link, &st);
		vty_out(vty, "  IPC link: %s, rx %u records, tx %u queued / %u written,"
			" last seq queued %u / written %u%s",
			st.state == AWORKER_IPC_LINK_READY ? "ready" :
			st.state == AWORKER_IPC_LINK_HELLO_WAIT ? "handshake" : "closed",
			st.rx_records, st.tx_queued, st.tx_written, st.last_queued_seq, st.last_written_seq,
			VTY_NEWLINE);
	}

	llist_for_each_entry(au, &g_pool->users, entry) {
		vty_out(vty, " SCCP user '%s': cs7 instance %u, SSN %u, %s%s", au->name, au->cs7_id, au->ssn,
			au->bound ? "bound" : (au->bind_sent ? "bind pending" : "no worker"), VTY_NEWLINE);
	}
}

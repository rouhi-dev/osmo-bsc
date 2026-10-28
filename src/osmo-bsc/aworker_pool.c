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
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
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

/* How long worker_kill_and_reap() waits for a worker to exit before it sends SIGKILL. The main process blocks for
 * at most this long, and only when a worker is being taken down. */
#define REAP_POLL_INTERVAL_US	50000
#define REAP_POLL_ATTEMPTS	20

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

	/*! cs7 instances this worker reported in CS7_INFO. */
	struct aworker_ipc_cs7_info cs7;
	bool cs7_reported;
};

struct aworker_pool {
	void *ctx;
	struct aworker_pool_cfg cfg;		/*!< copy taken at start time */
	unsigned int num;
	struct aworker *worker[AWORKER_POOL_MAX_WORKERS];
	struct llist_head users;
	uint32_t next_user_ref;
	struct osmo_timer_list startup_timer;
	bool no_fork;				/*!< started with aworker_pool_start_with_fds() */
	bool stopping;
};

static struct aworker_pool_cfg g_cfg = {
	.num_workers = 0,
	.startup_timeout = AWORKER_STARTUP_TIMEOUT_DEFAULT,
	.restart_delay = AWORKER_RESTART_DELAY_DEFAULT,
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

#define LOGW(w, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker %u (pid %ld): " fmt, (w)->idx, (long)(w)->pid, ## args)
#define LOGU(au, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker user '%s' (cs7 %u ssn %u): " fmt, (au)->name, (au)->cs7_id, (au)->ssn, ## args)

static const struct value_string state_names[] = {
	{ AWORKER_ST_STOPPED, "STOPPED" },
	{ AWORKER_ST_STARTING, "STARTING" },
	{ AWORKER_ST_READY, "READY" },
	{ AWORKER_ST_RESTARTING, "RESTARTING" },
	{ 0, NULL }
};

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

/* Collect the worker process. Sends SIGTERM, and SIGKILL if it does not exit. Blocks for at most
 * REAP_POLL_ATTEMPTS * REAP_POLL_INTERVAL_US, and only while a worker is being taken down.
 * \param[out] config_error  set to true if the worker exited with AWORKER_EXIT_CONFIG. */
static void worker_kill_and_reap(struct aworker *w, bool *config_error)
{
	unsigned int i;
	int status;

	if (config_error)
		*config_error = false;
	if (w->pid <= 0)
		return;

	for (i = 0; i < REAP_POLL_ATTEMPTS; i++) {
		pid_t rc = waitpid(w->pid, &status, WNOHANG);
		if (rc == w->pid) {
			if (WIFEXITED(status))
				LOGW(w, WEXITSTATUS(status) == AWORKER_EXIT_OK ? LOGL_INFO : LOGL_NOTICE,
				     "exited with status %u\n", WEXITSTATUS(status));
			else if (WIFSIGNALED(status))
				LOGW(w, LOGL_ERROR, "killed by signal %u\n", WTERMSIG(status));
			w->pid = 0;
			/* A configuration failure is permanent: restarting cannot help. */
			if (config_error && WIFEXITED(status) && WEXITSTATUS(status) == AWORKER_EXIT_CONFIG)
				*config_error = true;
			return;
		}
		if (rc < 0) {
			/* ECHILD: not our child (test mode) or already reaped */
			w->pid = 0;
			return;
		}
		if (i == 0)
			kill(w->pid, SIGTERM);
		usleep(REAP_POLL_INTERVAL_US);
	}
	LOGW(w, LOGL_ERROR, "did not exit after SIGTERM, sending SIGKILL\n");
	kill(w->pid, SIGKILL);
	waitpid(w->pid, &status, 0);
	w->pid = 0;
}

static void restart_timer_cb(void *data)
{
	struct aworker *w = data;

	if (w->pool->stopping)
		return;
	LOGW(w, LOGL_NOTICE, "restarting worker (start #%u)\n", w->starts + 1);
	if (worker_spawn(w) < 0) {
		LOGW(w, LOGL_ERROR, "restart failed, retrying in %u s\n", w->pool->cfg.restart_delay);
		osmo_timer_schedule(&w->restart_timer, w->pool->cfg.restart_delay, 0);
	}
}

/* The worker is gone: drop its link, collect the process, tell the users, then apply the failure policy. */
static void worker_lost(struct aworker *w, const char *why)
{
	struct aworker_pool *pool = w->pool;
	bool config_error = false;
	long pid = (long)w->pid;

	if (w->link) {
		aworker_ipc_link_free(w->link);
		w->link = NULL;
	}
	w->main_fd = -1;
	w->cs7_reported = false;
	memset(&w->cs7, 0, sizeof(w->cs7));
	w->state = AWORKER_ST_STOPPED;

	/* No SCCP operation is ever replayed: whatever was in flight towards this worker is treated as lost, and
	 * every SCCP connection of its cs7 instances is torn down by the users' 'unavailable' call-back. */
	users_lose_worker(w);
	worker_kill_and_reap(w, &config_error);

	if (pool->stopping)
		return;

	/* Started on existing sockets (aworker_pool_start_with_fds()): there is no process to restart and no
	 * configuration of our own to blame, so just leave the worker stopped. */
	if (pool->no_fork) {
		LOGP(DAWORKER, LOGL_ERROR, "aworker %u: worker lost (%s)\n", w->idx, why);
		return;
	}

	if (config_error) {
		LOGP(DAWORKER, LOGL_FATAL, "aworker %u (pid %ld): worker cannot start with its configuration"
		     " (%s); shutting down osmo-bsc\n", w->idx, pid, why);
		g_fatal = true;
		osmo_select_shutdown_request();
		return;
	}

	switch (pool->cfg.on_failure) {
	case AWORKER_FAILURE_SHUTDOWN:
		LOGP(DAWORKER, LOGL_FATAL, "aworker %u (pid %ld): worker lost (%s) and 'on-worker-failure"
		     " shutdown' is configured; shutting down osmo-bsc\n", w->idx, pid, why);
		g_fatal = true;
		osmo_select_shutdown_request();
		return;
	case AWORKER_FAILURE_RESTART:
	default:
		LOGP(DAWORKER, LOGL_ERROR, "aworker %u (pid %ld): worker lost (%s); restarting in %u s\n",
		     w->idx, pid, why, pool->cfg.restart_delay);
		w->state = AWORKER_ST_RESTARTING;
		osmo_timer_schedule(&w->restart_timer, pool->cfg.restart_delay, 0);
		return;
	}
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

static void handle_cs7_info(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_pool *pool = w->pool;
	unsigned int i, j;

	if (w->cs7_reported) {
		LOGW(w, LOGL_ERROR, "second CS7_INFO on the same link; closing link\n");
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
	return 0;
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

/* In the child, between fork() and execv(): close every descriptor but stdio and the IPC socket, so that no
 * socket, log file or database handle of the main process survives into the worker. */
static void child_close_fds(int keep_fd)
{
	int fds[256];
	unsigned int n = 0, i;
	struct dirent *de;
	DIR *d;

	d = opendir("/proc/self/fd");
	if (!d) {
		/* Fall back to a bounded sweep; OPEN_MAX is not necessarily meaningful, so use a sane ceiling. */
		for (i = 3; i < 1024; i++) {
			if ((int)i != keep_fd)
				close((int)i);
		}
		return;
	}
	while ((de = readdir(d)) && n < ARRAY_SIZE(fds)) {
		int fd = atoi(de->d_name);
		if (fd <= 2 || fd == keep_fd || fd == dirfd(d))
			continue;
		fds[n++] = fd;
	}
	closedir(d);
	for (i = 0; i < n; i++)
		close(fds[i]);
}

/* Run in the child after fork(); never returns. */
static void child_exec_worker(unsigned int idx, const char *cfg_file, int fd)
{
	char **argv;
	char fdstr[16], idxstr[16];
	unsigned int n = 0, i;
	int flags;

	/* Die with the main process, whatever happens to it -- including SIGKILL, which leaves no chance to
	 * send GOODBYE. This survives the execv() below. */
	prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);

	/* The IPC socket must survive execv(). */
	flags = fcntl(fd, F_GETFD);
	if (flags >= 0)
		fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);

	child_close_fds(fd);

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

	execv(g_exec_path, argv);
	/* execv() only returns on failure, and we have no usable logging left here. */
	fprintf(stderr, "osmo-bsc: aworker %u: cannot execute '%s': %s\n", idx, g_exec_path, strerror(errno));
	_exit(AWORKER_EXIT_CONFIG);
}

static int worker_spawn(struct aworker *w)
{
	struct aworker_pool *pool = w->pool;
	const char *cfg_file = pool->cfg.cfg_file[w->idx];
	int sp[2];
	pid_t pid;
	int rc;

	OSMO_ASSERT(!pool->no_fork);
	OSMO_ASSERT(cfg_file);
	OSMO_ASSERT(g_exec_path);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sp) < 0) {
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
		child_exec_worker(w->idx, cfg_file, sp[1]);
		/* not reached */
	}

	close(sp[1]);
	w->pid = pid;
	rc = link_create(w, sp[0]);
	if (rc < 0) {
		close(sp[0]);
		worker_kill_and_reap(w, NULL);
		return rc;
	}
	LOGW(w, LOGL_NOTICE, "started '%s' with config file '%s'\n", g_exec_path, cfg_file);
	return 0;
}

static void startup_timer_cb(void *data)
{
	struct aworker_pool *pool = data;
	struct aworker_user *au;
	unsigned int i;

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		if (w->state == AWORKER_ST_READY)
			continue;
		LOGW(w, LOGL_ERROR, "not ready %u s after start (state %s); treating the worker as failed\n",
		     pool->cfg.startup_timeout, get_value_string(state_names, w->state));
		if (w->link) {
			/* closed_cb -> worker_lost() applies the configured failure policy. */
			aworker_ipc_link_close(w->link);
		} else if (w->state != AWORKER_ST_RESTARTING) {
			worker_lost(w, "startup timeout");
		}
	}

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
	pool->cfg = g_cfg;
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
		pool->worker[i] = w;
	}
	return pool;
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
	     " startup-timeout %u s, restart-delay %u s\n", pool->num,
	     get_value_string(aworker_failure_policy_names, pool->cfg.on_failure), pool->cfg.startup_timeout,
	     pool->cfg.restart_delay);
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

void aworker_pool_stop(void)
{
	struct aworker_pool *pool = g_pool;
	struct aworker_user *au;
	unsigned int i;

	if (!pool)
		return;
	pool->stopping = true;
	osmo_timer_del(&pool->startup_timer);

	for (i = 0; i < pool->num; i++) {
		struct aworker *w = pool->worker[i];
		struct msgb *rec;

		osmo_timer_del(&w->restart_timer);
		if (w->link) {
			if (aworker_ipc_link_get_state(w->link) == AWORKER_IPC_LINK_READY
			    && aworker_ipc_enc_goodbye(pool->ctx, &rec, AWORKER_IPC_GOODBYE_SHUTDOWN) == 0)
				aworker_ipc_link_send(w->link, rec, NULL);
			aworker_ipc_link_free(w->link);
			w->link = NULL;
		}
		w->main_fd = -1;
		w->state = AWORKER_ST_STOPPED;
		worker_kill_and_reap(w, NULL);
	}

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
		" restart-delay %u s%s", g_pool->num,
		get_value_string(aworker_failure_policy_names, g_pool->cfg.on_failure), g_pool->cfg.startup_timeout,
		g_pool->cfg.restart_delay, VTY_NEWLINE);

	for (i = 0; i < g_pool->num; i++) {
		struct aworker *w = g_pool->worker[i];
		struct aworker_ipc_link_stats st;

		vty_out(vty, " worker %u: state %s, pid %ld, starts %u, config file %s%s", w->idx,
			get_value_string(state_names, w->state), (long)w->pid, w->starts,
			g_pool->cfg.cfg_file[i] ? g_pool->cfg.cfg_file[i] : "(none)", VTY_NEWLINE);
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

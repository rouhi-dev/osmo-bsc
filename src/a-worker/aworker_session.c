/* A-worker Supervisor: the session socket, BSC sessions and the operator channel (S2-5; see aworker_session.h) */
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

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_canon.h>
#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/aworker_pool_internal.h>
#include <osmocom/bsc/aworker_session.h>

#ifndef F_GET_SEALS
#define F_GET_SEALS	1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL	0x0001
#define F_SEAL_SHRINK	0x0002
#define F_SEAL_GROW	0x0004
#define F_SEAL_WRITE	0x0008
#endif

#define FLUSH_MS	500

enum sess_state {
	S_NONE,
	S_ACTIVE,
	S_UNRESPONSIVE,
	S_FENCING,
	S_CLOSING,
	S_CLOSED,
};

static const struct value_string sess_state_names[] = {
	{ S_NONE, "NONE" },
	{ S_ACTIVE, "ACTIVE" },
	{ S_UNRESPONSIVE, "UNRESPONSIVE" },
	{ S_FENCING, "FENCING" },
	{ S_CLOSING, "CLOSING" },
	{ S_CLOSED, "CLOSED" },
	{ 0, NULL }
};

enum conn_kind {
	CK_NEW,		/* nothing received yet */
	CK_SESSION,	/* the session's BSC */
	CK_OPERATOR,
	CK_REJECTED,	/* a BSC whose SESSION_OPEN was refused: kept until it goes or the linger time is over */
};

struct conn {
	struct llist_head entry;
	struct aworker_ipc_link *link;	/* allocated under g_s.ctx, not under the conn: see conn_free() */
	int sock;			/* the link's descriptor (owned by the link), for MSG_PEEK */
	enum conn_kind kind;
	pid_t pid;
	uid_t uid;
	struct osmo_timer_list timer;	/* first record / reject linger */
};

/* The session side of one worker. */
struct sworker {
	uint8_t att;			/* enum aworker_ipc_attach_state: NONE, ATTACHING, ATTACHED, DETACHING */
	uint32_t session;		/* while not NONE */
	uint32_t inc;			/* the incarnation attached */
	uint32_t up_seq;		/* WORKER_UP's sequence number on the session link */
	bool state_ok;			/* STATE(attach-ok) received */
	struct osmo_timer_list retry;	/* attach (again) */
	struct osmo_timer_list ttimer;	/* WORKER_UP written? / delayed DETACH */
};

static struct {
	void *ctx;
	bool started;
	bool stopping;
	char *path;
	char *lock_path;
	int lock_fd;
	struct osmo_fd lfd;
	struct llist_head conns;
	unsigned int num_conns;
	struct aworker_session_test_hooks th;
	char *doc;
	size_t doc_len;

	enum sess_state state;
	uint32_t id;			/* the current (or last) session */
	uint32_t counter;
	struct conn *conn;		/* the session's BSC, while ACTIVE/UNRESPONSIVE/FENCING */
	pid_t pid;
	uid_t uid;
	int pidfd;
	struct osmo_fd pidofd;
	bool fence_failed;
	char fence_why[160];
	struct conn *fence_op;		/* the operator waiting for the fence's outcome */
	struct osmo_timer_list echo_timer;
	struct osmo_timer_list fence_timer;
	uint32_t echo_token;
	bool echo_outstanding;
	unsigned int misses;
	unsigned int closing_entries;
	struct sworker w[AWORKER_POOL_MAX_WORKERS];
} g_s = { .lock_fd = -1, .pidfd = -1 };

#define LOGS(lvl, fmt, args...) LOGPAW(lvl, "session: " fmt, ## args)

static void conn_free(struct conn *c);
static void session_close(const char *why);
static void attach_worker(unsigned int idx);
static void check_closed(void);

/***********************************************************************
 * Helpers
 ***********************************************************************/

static struct aworker *pool_worker(unsigned int idx)
{
	struct aworker_pool *pool = aworker_pool_get();

	if (!pool || idx >= pool->num)
		return NULL;
	return pool->worker[idx];
}

static unsigned int pool_num(void)
{
	struct aworker_pool *pool = aworker_pool_get();
	return pool ? pool->num : 0;
}

static bool session_live(void)
{
	return g_s.state == S_ACTIVE || g_s.state == S_UNRESPONSIVE || g_s.state == S_FENCING;
}

static int conn_send(struct conn *c, struct msgb *msg, int fd, uint32_t *seq)
{
	if (!c || !c->link) {
		msgb_free(msg);
		if (fd >= 0)
			close(fd);
		return -ENOTCONN;
	}
	return fd >= 0 ? aworker_ipc_link_send_fd(c->link, msg, fd, seq) : aworker_ipc_link_send(c->link, msg, seq);
}

static void op_result(struct conn *c, uint8_t op, int rc, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void op_result(struct conn *c, uint8_t op, int rc, const char *fmt, ...)
{
	char text[AWORKER_IPC_MAX_TEXT + 1];
	struct msgb *msg;
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	if (aworker_ipc_enc_op_result(g_s.ctx, &msg, op, rc, text) == 0)
		conn_send(c, msg, -1, NULL);
}

static void to_bsc_worker_down(unsigned int idx, uint8_t reason, uint32_t inc)
{
	struct msgb *msg;

	if (!g_s.conn || !inc)
		return;
	if (aworker_ipc_enc_worker_down(g_s.ctx, &msg, idx, reason, inc, g_s.id) == 0)
		conn_send(g_s.conn, msg, -1, NULL);
}

static void to_bsc_worker_state(unsigned int idx)
{
	struct aworker *w = pool_worker(idx);
	struct sworker *sw = &g_s.w[idx];
	struct msgb *msg;

	if (!g_s.conn || !w)
		return;
	if (aworker_ipc_enc_worker_state(g_s.ctx, &msg, idx, w->state, sw->att,
					 sw->att != AWORKER_IPC_ATT_NONE ? sw->inc : w->starts,
					 sw->att != AWORKER_IPC_ATT_NONE ? sw->session : 0) == 0)
		conn_send(g_s.conn, msg, -1, NULL);
}

/***********************************************************************
 * Attaching and detaching workers
 ***********************************************************************/

static void send_detach(unsigned int idx)
{
	struct aworker *w = pool_worker(idx);
	struct sworker *sw = &g_s.w[idx];
	struct msgb *msg;

	if (sw->att != AWORKER_IPC_ATT_DETACHING || !w || !w->link)
		return;
	if (aworker_ipc_enc_detach(g_s.ctx, &msg, sw->session) == 0
	    && aworker_ipc_link_send(w->link, msg, NULL) == 0)
		LOGS(LOGL_INFO, "DETACH(session %u) sent to worker %u\n", sw->session, idx);
}

static void ttimer_cb(void *data)
{
	unsigned int idx = (struct sworker *)data - g_s.w;
	struct sworker *sw = data;

	if (sw->att == AWORKER_IPC_ATT_DETACHING) {
		send_detach(idx);
		return;
	}
	if (sw->att == AWORKER_IPC_ATT_ATTACHING) {
		/* see check_attached() */
		struct aworker_ipc_link_stats st;
		if (!g_s.conn)
			return;
		aworker_ipc_link_get_stats(g_s.conn->link, &st);
		if (st.last_written_seq >= sw->up_seq) {
			sw->att = AWORKER_IPC_ATT_ATTACHED;
			LOGS(LOGL_NOTICE, "worker %u (incarnation %u) attached to session %u\n", idx, sw->inc, sw->session);
			to_bsc_worker_state(idx);
		} else {
			osmo_timer_schedule(&sw->ttimer, 0, 10000);
		}
	}
}

/* ATTACHED needs the worker's STATE(attach-ok) and the WORKER_UP written: only then the Supervisor's copy of the BSC
 * end is closed, so that a BSC death is an EOF at the worker (plan 14.3, clarification A). */
static void check_attached(unsigned int idx)
{
	struct sworker *sw = &g_s.w[idx];

	if (sw->att != AWORKER_IPC_ATT_ATTACHING || !sw->state_ok)
		return;
	ttimer_cb(sw);
}

static void retry_cb(void *data)
{
	attach_worker((struct sworker *)data - g_s.w);
}

static void attach_worker(unsigned int idx)
{
	struct aworker *w = pool_worker(idx);
	struct sworker *sw = &g_s.w[idx];
	struct msgb *msg;
	uint32_t seq = 0;
	int sp[2], rc;

	if (!g_s.conn || (g_s.state != S_ACTIVE && g_s.state != S_UNRESPONSIVE) || g_s.stopping)
		return;
	if (!w || w->state != AWORKER_ST_READY || !w->link || sw->att != AWORKER_IPC_ATT_NONE)
		return;
	osmo_timer_del(&sw->retry);
	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) < 0) {
		LOGS(LOGL_ERROR, "worker %u: socketpair() failed: %s; trying again\n", idx, strerror(errno));
		osmo_timer_schedule(&sw->retry, 0, AWORKER_SESSION_REATTACH_MS * 1000);
		return;
	}
	sw->att = AWORKER_IPC_ATT_ATTACHING;
	sw->session = g_s.id;
	sw->inc = w->starts;
	sw->state_ok = false;
	/* sp[0] to the worker, sp[1] to the BSC. Once handed to a link, a descriptor is the link's in every case (it
	 * closes it on failure); before that, it is closed here. */
	rc = aworker_ipc_enc_attach(g_s.ctx, &msg, g_s.id, sw->inc);
	if (rc < 0)
		close(sp[0]);
	else
		rc = aworker_ipc_link_send_fd(w->link, msg, sp[0], NULL);
	if (rc < 0) {
		close(sp[1]);
		sw->att = AWORKER_IPC_ATT_NONE;
		LOGS(LOGL_ERROR, "worker %u: cannot send ATTACH: %s\n", idx, strerror(-rc));
		return;
	}
	rc = aworker_ipc_enc_worker_up(g_s.ctx, &msg, idx, sw->inc, g_s.id, &w->cs7);
	if (rc < 0)
		close(sp[1]);
	else
		rc = conn_send(g_s.conn, msg, sp[1], &seq);
	if (rc < 0) {
		LOGS(LOGL_ERROR, "worker %u: cannot send WORKER_UP to the BSC: %s; detaching it\n", idx, strerror(-rc));
		sw->att = AWORKER_IPC_ATT_DETACHING;
		send_detach(idx);
		return;
	}
	sw->up_seq = seq;
	LOGS(LOGL_INFO, "worker %u (incarnation %u): ATTACH and WORKER_UP sent for session %u\n", idx, sw->inc,
	     g_s.id);
}

static void session_worker_ready(struct aworker *w)
{
	attach_worker(w->idx);
}

static void session_worker_lost(struct aworker *w)
{
	struct sworker *sw = &g_s.w[w->idx];
	uint8_t was = sw->att;

	osmo_timer_del(&sw->retry);
	osmo_timer_del(&sw->ttimer);
	if (was == AWORKER_IPC_ATT_NONE)
		return;
	sw->att = AWORKER_IPC_ATT_NONE;
	LOGS(LOGL_NOTICE, "worker %u (incarnation %u) lost while %s session %u\n", w->idx, sw->inc,
	     was == AWORKER_IPC_ATT_DETACHING ? "detaching from" : "attached to", sw->session);
	if (session_live() && sw->session == g_s.id && was != AWORKER_IPC_ATT_DETACHING)
		to_bsc_worker_down(w->idx, AWORKER_IPC_DOWN_LOST, sw->inc);
	check_closed();
}

static void session_rx_state(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct sworker *sw = &g_s.w[w->idx];
	uint8_t cause = m->u.state.cause;

	LOGS(LOGL_INFO, "worker %u: STATE session %u %s (%s), %u user(s) unbound\n", w->idx, m->u.state.session_id,
	     aworker_ipc_attach_state_name(m->u.state.state), aworker_ipc_state_cause_name(cause),
	     m->u.state.users_unbound);
	if (sw->att == AWORKER_IPC_ATT_NONE || m->u.state.session_id != sw->session) {
		LOGS(LOGL_INFO, "worker %u: STATE for session %u is stale; ignored\n", w->idx, m->u.state.session_id);
		return;
	}
	switch (cause) {
	case AWORKER_IPC_SC_ATTACH_OK:
		if (sw->att == AWORKER_IPC_ATT_ATTACHING) {
			sw->state_ok = true;
			check_attached(w->idx);
		}
		return;
	case AWORKER_IPC_SC_ATTACH_REFUSED:
		LOGS(LOGL_ERROR, "worker %u refused ATTACH to session %u\n", w->idx, sw->session);
		sw->att = AWORKER_IPC_ATT_NONE;
		osmo_timer_del(&sw->ttimer);
		if (session_live() && sw->session == g_s.id) {
			to_bsc_worker_down(w->idx, AWORKER_IPC_DOWN_ATTACH_FAILED, sw->inc);
			osmo_timer_schedule(&sw->retry, 0, AWORKER_SESSION_REATTACH_MS * 1000);
		}
		check_closed();
		return;
	default:
		if (m->u.state.state != AWORKER_IPC_ATT_DETACHED)
			return;
		if (cause != AWORKER_IPC_SC_DETACH_REQ)
			LOGS(LOGL_NOTICE, "worker %u detached itself from session %u: %s\n", w->idx, sw->session,
			     aworker_ipc_state_cause_name(cause));
		else
			LOGS(LOGL_INFO, "worker %u detached from session %u\n", w->idx, sw->session);
		sw->att = AWORKER_IPC_ATT_NONE;
		osmo_timer_del(&sw->ttimer);
		if (session_live() && sw->session == g_s.id) {
			to_bsc_worker_down(w->idx, AWORKER_IPC_DOWN_DETACHED, sw->inc);
			osmo_timer_schedule(&sw->retry, 0, AWORKER_SESSION_REATTACH_MS * 1000);
		}
		check_closed();
		return;
	}
}

static const struct aworker_pool_session_hooks session_hooks = {
	.worker_ready = session_worker_ready,
	.worker_lost = session_worker_lost,
	.rx_state = session_rx_state,
};

/***********************************************************************
 * The session state machine
 ***********************************************************************/

static void pidfd_release(void)
{
	if (g_s.pidofd.fd >= 0) {
		osmo_fd_unregister(&g_s.pidofd);
		g_s.pidofd.fd = -1;
	}
	if (g_s.pidfd >= 0)
		close(g_s.pidfd);
	g_s.pidfd = -1;
}

static void check_closed(void)
{
	unsigned int i;

	if (g_s.state != S_CLOSING)
		return;
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (g_s.w[i].att != AWORKER_IPC_ATT_NONE)
			return;
	}
	g_s.state = S_CLOSED;
	LOGS(LOGL_NOTICE, "session %u: CLOSED (every worker detached or gone)\n", g_s.id);
}

/* CLOSING: the entry action, exactly once per session (SUP 9.1). */
static void session_close(const char *why)
{
	struct conn *c;
	unsigned int i;

	if (!session_live())
		return;
	g_s.closing_entries++;
	LOGS(LOGL_NOTICE, "session %u: CLOSING (%s)\n", g_s.id, why);
	g_s.state = S_CLOSING;
	osmo_timer_del(&g_s.echo_timer);
	osmo_timer_del(&g_s.fence_timer);
	pidfd_release();
	c = g_s.conn;
	g_s.conn = NULL;
	if (c)
		conn_free(c);
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		struct sworker *sw = &g_s.w[i];
		osmo_timer_del(&sw->retry);
		if ((sw->att != AWORKER_IPC_ATT_ATTACHING && sw->att != AWORKER_IPC_ATT_ATTACHED)
		    || sw->session != g_s.id)
			continue;
		sw->att = AWORKER_IPC_ATT_DETACHING;
		osmo_timer_del(&sw->ttimer);
		if (g_s.th.detach_delay_ms)
			osmo_timer_schedule(&sw->ttimer, g_s.th.detach_delay_ms / 1000,
					    (g_s.th.detach_delay_ms % 1000) * 1000);
		else
			send_detach(i);
	}
	if (g_s.fence_op) {
		op_result(g_s.fence_op, AWORKER_IPC_OP_FENCE_SESSION, 0, "session %u fenced: %s", g_s.id, why);
		g_s.fence_op = NULL;
	}
	check_closed();
}

static void echo_timer_cb(void *data)
{
	struct msgb *msg;

	if (g_s.state != S_ACTIVE && g_s.state != S_UNRESPONSIVE)
		return;
	if (g_s.echo_outstanding) {
		g_s.misses++;
		if (g_s.state == S_ACTIVE) {
			g_s.state = S_UNRESPONSIVE;
			LOGS(LOGL_NOTICE, "session %u: UNRESPONSIVE (ECHO %u unanswered); the workers and their data"
			     " links are left as they are\n", g_s.id, g_s.echo_token);
		}
		if (g_s.misses == aworker_pool_cfg()->session_echo_misses)
			LOGS(LOGL_ERROR, "session %u: %u session ECHOs unanswered in a row (session-echo-misses): no"
			     " action on the session or the workers (D-S12); it ends by an answer, the BSC's end, or"
			     " fencing\n", g_s.id, g_s.misses);
	}
	if (aworker_ipc_enc_echo(g_s.ctx, &msg, false, ++g_s.echo_token) == 0
	    && conn_send(g_s.conn, msg, -1, NULL) == 0)
		g_s.echo_outstanding = true;
	osmo_timer_schedule(&g_s.echo_timer, aworker_pool_cfg()->session_echo_interval, 0);
}

static void echo_rsp(uint32_t token)
{
	if (token != g_s.echo_token || !g_s.echo_outstanding)
		return;
	g_s.echo_outstanding = false;
	g_s.misses = 0;
	if (g_s.state == S_UNRESPONSIVE) {
		g_s.state = S_ACTIVE;
		LOGS(LOGL_NOTICE, "session %u: ACTIVE again (ECHO answered)\n", g_s.id);
	}
}

/***********************************************************************
 * Fencing
 ***********************************************************************/

static void fence_fail(const char *why)
{
	g_s.fence_failed = true;
	snprintf(g_s.fence_why, sizeof(g_s.fence_why), "%s", why);
	LOGS(LOGL_ERROR, "session %u: FENCE_FAILED: %s. Nothing is admitted until it is resolved (FENCE_SESSION again,"
	     " or the BSC's end)\n", g_s.id, why);
	if (g_s.fence_op) {
		op_result(g_s.fence_op, AWORKER_IPC_OP_FENCE_SESSION, -ETIMEDOUT, "session %u: FENCE_FAILED: %s",
			  g_s.id, why);
		g_s.fence_op = NULL;
	}
}

static void fence_timer_cb(void *data)
{
	char why[96];

	if (g_s.state != S_FENCING)
		return;
	snprintf(why, sizeof(why), "the exit of BSC pid %ld was not confirmed within %u ms", (long)g_s.pid,
		 AWORKER_SESSION_FENCE_CONFIRM_MS);
	fence_fail(why);
}

static int pidfd_cb(struct osmo_fd *ofd, unsigned int what)
{
	char why[64];

	/* Readable: the process has exited. */
	snprintf(why, sizeof(why), "BSC pid %ld exit confirmed", (long)g_s.pid);
	osmo_fd_unregister(ofd);
	ofd->fd = -1;
	if (g_s.state == S_FENCING)
		session_close(why);
	return 0;
}

static void fence(struct conn *op)
{
	int sig = g_s.th.fence_signal >= 0 ? g_s.th.fence_signal : SIGKILL;

	if (g_s.state == S_FENCING && !g_s.fence_failed) {
		op_result(op, AWORKER_IPC_OP_FENCE_SESSION, -EBUSY, "session %u: fencing is in progress", g_s.id);
		return;
	}
	if (g_s.state != S_ACTIVE && g_s.state != S_UNRESPONSIVE && g_s.state != S_FENCING) {
		op_result(op, AWORKER_IPC_OP_FENCE_SESSION, -ENOENT, "no session to fence (%s)",
			  get_value_string(sess_state_names, g_s.state));
		return;
	}
	if (g_s.pidfd < 0) {
		LOGS(LOGL_ERROR, "session %u: fencing refused: no pidfd of BSC pid %ld\n", g_s.id, (long)g_s.pid);
		op_result(op, AWORKER_IPC_OP_FENCE_SESSION, -ENOTSUP, "session %u cannot be fenced: there is no pidfd of"
			  " BSC pid %ld (fencing refused; the session is unchanged)", g_s.id, (long)g_s.pid);
		return;
	}
	LOGS(LOGL_NOTICE, "session %u: FENCING BSC pid %ld (signal %d)\n", g_s.id, (long)g_s.pid, sig);
	g_s.state = S_FENCING;
	g_s.fence_failed = false;
	g_s.fence_op = op;
	osmo_timer_del(&g_s.echo_timer);
	if (g_s.pidofd.fd < 0) {
		osmo_fd_setup(&g_s.pidofd, g_s.pidfd, OSMO_FD_READ, pidfd_cb, NULL, 0);
		if (osmo_fd_register(&g_s.pidofd) < 0)
			g_s.pidofd.fd = -1;
	}
	if (syscall(SYS_pidfd_send_signal, g_s.pidfd, sig, NULL, 0) < 0) {
		char why[96];
		snprintf(why, sizeof(why), "pidfd_send_signal(): %s", strerror(errno));
		fence_fail(why);
		return;
	}
	osmo_timer_schedule(&g_s.fence_timer, AWORKER_SESSION_FENCE_CONFIRM_MS / 1000,
			    (AWORKER_SESSION_FENCE_CONFIRM_MS % 1000) * 1000);
}

/***********************************************************************
 * SESSION_OPEN
 ***********************************************************************/

static void reject(struct conn *c, uint8_t reason, const char *detail)
{
	struct msgb *msg;

	LOGS(LOGL_NOTICE, "SESSION_OPEN of pid %ld refused (%s): %s\n", (long)c->pid,
	     aworker_ipc_reject_reason_name(reason), detail);
	c->kind = CK_REJECTED;
	if (aworker_ipc_enc_session_reject(g_s.ctx, &msg, reason, detail) == 0)
		conn_send(c, msg, -1, NULL);
	osmo_timer_schedule(&c->timer, AWORKER_SESSION_REJECT_LINGER_S, 0);
}

/* The BSC's canonical document: a memfd with all four seals, exactly doc_len octets. Closes fd. */
static char *doc_read(int fd, uint32_t doc_len, char *why, size_t why_len)
{
	struct stat st;
	char *buf = NULL;
	ssize_t n;
	int seals;

	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		snprintf(why, why_len, "the document's descriptor is not a regular file");
		goto out;
	}
	seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || (seals & (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE))
			 != (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)) {
		snprintf(why, why_len, "the document is not a memfd with all four seals");
		goto out;
	}
	if (doc_len > AWORKER_CANON_MAX || (uint64_t)st.st_size != doc_len) {
		snprintf(why, why_len, "the document has %lld octets, SESSION_OPEN says %u (at most %u)",
			 (long long)st.st_size, doc_len, AWORKER_CANON_MAX);
		goto out;
	}
	buf = talloc_size(g_s.ctx, doc_len + 1);
	if (!buf) {
		snprintf(why, why_len, "out of memory");
		goto out;
	}
	n = pread(fd, buf, doc_len, 0);
	if (n != (ssize_t)doc_len) {
		snprintf(why, why_len, "the document cannot be read completely");
		TALLOC_FREE(buf);
		goto out;
	}
	buf[doc_len] = '\0';
out:
	close(fd);
	return buf;
}

static bool any_worker_attached(unsigned int *idx)
{
	unsigned int i;

	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (g_s.w[i].att != AWORKER_IPC_ATT_NONE) {
			*idx = i;
			return true;
		}
	}
	return false;
}

static void handle_open(struct conn *c, const struct aworker_ipc_msg *m, int fd)
{
	char why[AWORKER_IPC_MAX_TEXT];
	struct msgb *msg;
	unsigned int i;
	char *doc;
	char b;

	osmo_timer_del(&c->timer);
	c->kind = CK_REJECTED;
	if (g_s.stopping) {
		close(fd);
		reject(c, AWORKER_IPC_REJ_NO_SESSIONS, "the pool is shutting down");
		return;
	}
	if (m->u.session_open.version != AWORKER_IPC_SESSION_VERSION) {
		close(fd);
		snprintf(why, sizeof(why), "session protocol version %u asked for; this Supervisor speaks %u",
			 m->u.session_open.version, AWORKER_IPC_SESSION_VERSION);
		reject(c, AWORKER_IPC_REJ_VERSION, why);
		return;
	}
	if (g_s.state != S_NONE && g_s.state != S_CLOSED) {
		close(fd);
		snprintf(why, sizeof(why), "session %u is %s%s", g_s.id, get_value_string(sess_state_names, g_s.state),
			 g_s.state == S_FENCING && g_s.fence_failed ? " (FENCE_FAILED)" : "");
		reject(c, AWORKER_IPC_REJ_BUSY, why);
		return;
	}
	if (any_worker_attached(&i)) {
		close(fd);
		snprintf(why, sizeof(why), "worker %u has not detached from session %u yet", i, g_s.w[i].session);
		reject(c, AWORKER_IPC_REJ_BUSY, why);
		return;
	}
	doc = doc_read(fd, m->u.session_open.doc_len, why, sizeof(why));
	if (!doc) {
		reject(c, AWORKER_IPC_REJ_BAD_DOCUMENT, why);
		return;
	}
	if (aworker_canon_compare(g_s.doc, g_s.doc_len, doc, m->u.session_open.doc_len, why, sizeof(why))) {
		talloc_free(doc);
		reject(c, AWORKER_IPC_REJ_CONFIG_MISMATCH, why);
		return;
	}
	talloc_free(doc);

	/* Admitted. The BSC's identity for fencing: its pid (SO_PEERCRED, at connect time) and a pidfd of it. The
	 * pidfd is only trusted if the connection is still alive afterwards: a dead peer's end would read as EOF, and its
	 * pid might be another process' by now. */
	pidfd_release();
	if (!g_s.th.no_pidfd) {
		g_s.pidfd = syscall(SYS_pidfd_open, c->pid, 0);
		if (g_s.pidfd >= 0 && recv(c->sock, &b, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
			close(g_s.pidfd);
			g_s.pidfd = -1;
		}
	}
	g_s.id = ++g_s.counter;
	g_s.state = S_ACTIVE;
	g_s.conn = c;
	g_s.pid = c->pid;
	g_s.uid = c->uid;
	g_s.fence_failed = false;
	g_s.echo_outstanding = false;
	g_s.misses = 0;
	c->kind = CK_SESSION;
	LOGS(LOGL_NOTICE, "session %u: ACTIVE (BSC pid %ld, uid %u; fencing %s)\n", g_s.id, (long)c->pid,
	     (unsigned int)c->uid, g_s.pidfd >= 0 ? "available" : "UNAVAILABLE (no pidfd)");
	if (aworker_ipc_enc_session_accept(g_s.ctx, &msg, g_s.id, aworker_pool_get()->boot_nonce, pool_num()) == 0)
		conn_send(c, msg, -1, NULL);
	osmo_timer_schedule(&g_s.echo_timer, aworker_pool_cfg()->session_echo_interval, 0);
	for (i = 0; i < pool_num(); i++)
		attach_worker(i);
}

/***********************************************************************
 * The operator channel
 ***********************************************************************/

static void status(struct conn *op)
{
	char text[AWORKER_IPC_MAX_TEXT + 1];
	size_t n = 0;
	unsigned int i;

#define ADD(fmt, args...) do { if (n < sizeof(text)) n += snprintf(text + n, sizeof(text) - n, fmt, ## args); } while (0)
	ADD("pool: %u worker(s)\n", pool_num());
	ADD("session: %s", get_value_string(sess_state_names, g_s.state));
	if (g_s.state != S_NONE)
		ADD(", id %u, BSC pid %ld uid %u", g_s.id, (long)g_s.pid, (unsigned int)g_s.uid);
	if (session_live())
		ADD(", fencing %s, ECHO misses %u", g_s.pidfd >= 0 ? "available" : "unavailable", g_s.misses);
	if (g_s.state == S_FENCING && g_s.fence_failed)
		ADD(", FENCE_FAILED: %s", g_s.fence_why);
	ADD("\nCLOSING entered: %u\n", g_s.closing_entries);
	for (i = 0; i < pool_num(); i++) {
		struct aworker_pool_worker_info info;
		struct sworker *sw = &g_s.w[i];
		if (aworker_pool_worker_info(i, &info) < 0)
			continue;
		ADD("worker %u: %s, pid %ld, incarnation %u, %s", i, aworker_state_name(info.state), (long)info.pid,
		    info.starts, aworker_ipc_attach_state_name(sw->att));
		if (sw->att != AWORKER_IPC_ATT_NONE)
			ADD(" session %u", sw->session);
		ADD("\n");
	}
#undef ADD
	op_result(op, AWORKER_IPC_OP_STATUS, 0, "%s", text);
}

static void handle_op(struct conn *c, const struct aworker_ipc_msg *m)
{
	int rc;

	osmo_timer_del(&c->timer);
	c->kind = CK_OPERATOR;
	LOGS(LOGL_NOTICE, "operator pid %ld: %s %u\n", (long)c->pid, aworker_ipc_op_name(m->u.op_request.op),
	     m->u.op_request.arg);
	switch (m->u.op_request.op) {
	case AWORKER_IPC_OP_STATUS:
		status(c);
		return;
	case AWORKER_IPC_OP_RESET_WORKER:
		rc = aworker_pool_reset_worker(m->u.op_request.arg);
		op_result(c, AWORKER_IPC_OP_RESET_WORKER, rc, "%s", rc == 0 ? "worker reset; starting it again"
			  : rc == -EBUSY ? "the worker is not FAILED; nothing to reset"
			  : rc == -EINVAL ? "there is no such worker" : "the pool is not running");
		return;
	case AWORKER_IPC_OP_FENCE_SESSION:
		fence(c);
		return;
	case AWORKER_IPC_OP_POOL_SHUTDOWN:
		op_result(c, AWORKER_IPC_OP_POOL_SHUTDOWN, 0, "shutting the pool down");
		LOGS(LOGL_NOTICE, "pool shutdown requested by operator pid %ld\n", (long)c->pid);
		osmo_select_shutdown_request();
		return;
	}
}

/***********************************************************************
 * Connections
 ***********************************************************************/

static void conn_free(struct conn *c)
{
	if (c == g_s.conn)
		g_s.conn = NULL;
	if (c == g_s.fence_op)
		g_s.fence_op = NULL;
	osmo_timer_del(&c->timer);
	llist_del(&c->entry);
	g_s.num_conns--;
	/* Freed even from within its own call-back: the link defers that, and lives under g_s.ctx, not under c. */
	if (c->link)
		aworker_ipc_link_free(c->link);
	talloc_free(c);
}

static void conn_timer_cb(void *data)
{
	struct conn *c = data;

	if (c->kind == CK_NEW)
		LOGS(LOGL_NOTICE, "connection of pid %ld sent nothing within %u s; closed\n", (long)c->pid,
		     AWORKER_SESSION_FIRST_RECORD_S);
	conn_free(c);
}

static void conn_rx_cb(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	struct conn *c = aworker_ipc_link_get_priv(link);

	switch (m->type) {
	case AWORKER_IPC_MT_SESSION_OPEN:
		if (c->kind == CK_NEW) {
			handle_open(c, m, aworker_ipc_link_take_fd(link));
			return;
		}
		break;
	case AWORKER_IPC_MT_OP_REQUEST:
		if (c->kind == CK_NEW || c->kind == CK_OPERATOR) {
			handle_op(c, m);
			return;
		}
		break;
	case AWORKER_IPC_MT_SESSION_CLOSE:
		if (c->kind == CK_SESSION) {
			session_close("the BSC closed it");
			return;
		}
		break;
	case AWORKER_IPC_MT_ECHO_RSP:
		if (c->kind == CK_SESSION) {
			echo_rsp(m->u.echo_token);
			return;
		}
		break;
	default:
		break;
	}
	LOGS(LOGL_ERROR, "%s on a connection of pid %ld that is %s; closing it\n", aworker_ipc_msg_type_name(m->type),
	     (long)c->pid, c->kind == CK_NEW ? "new" : c->kind == CK_SESSION ? "a session" : c->kind == CK_OPERATOR
	     ? "an operator's" : "refused");
	if (c->kind == CK_SESSION)
		session_close("protocol violation by the BSC");
	else
		conn_free(c);
}

static void conn_closed_cb(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	struct conn *c = aworker_ipc_link_get_priv(link);
	char why[96];

	if (c->kind == CK_SESSION && c == g_s.conn) {
		snprintf(why, sizeof(why), "connection to the BSC: %s",
			 info->reason == AWORKER_IPC_CLOSE_PEER_EOF ? "EOF" : aworker_ipc_close_reason_name(info->reason));
		session_close(why);	/* frees c */
		return;
	}
	conn_free(c);
}

static const struct aworker_ipc_link_ops conn_ops = {
	.rx_cb = conn_rx_cb,
	.closed_cb = conn_closed_cb,
};

static int listen_cb(struct osmo_fd *ofd, unsigned int what)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_SUPERVISOR,
		.worker_idx = 0,
		.pid = (uint32_t)getpid(),
		.ident = aworker_program_name,
		.log_cat = aworker_log_cat,
		.classes = AWORKER_IPC_CLASS_SESSION,
		.fd_passing = true,
	};
	struct ucred cr;
	socklen_t l = sizeof(cr);
	uid_t own = g_s.th.peer_uid >= 0 ? (uid_t)g_s.th.peer_uid : geteuid();
	struct conn *c;
	int fd;

	fd = accept4(ofd->fd, NULL, NULL, SOCK_CLOEXEC);
	if (fd < 0)
		return 0;
	/* N5: before anything is read from it. */
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) < 0) {
		LOGS(LOGL_ERROR, "connection refused: no peer credentials (%s)\n", strerror(errno));
		close(fd);
		return 0;
	}
	if (cr.uid != own && cr.uid != 0) {
		LOGS(LOGL_ERROR, "connection of pid %ld refused: uid %u; only uid %u or root may connect (N5)\n",
		     (long)cr.pid, (unsigned int)cr.uid, (unsigned int)own);
		close(fd);
		return 0;
	}
	if (g_s.num_conns >= AWORKER_SESSION_MAX_CONNS || g_s.stopping) {
		LOGS(LOGL_ERROR, "connection of pid %ld refused: %s\n", (long)cr.pid,
		     g_s.stopping ? "stopping" : "too many connections");
		close(fd);
		return 0;
	}
	c = talloc_zero(g_s.ctx, struct conn);
	if (!c) {
		close(fd);
		return 0;
	}
	c->pid = cr.pid;
	c->uid = cr.uid;
	c->sock = fd;
	c->kind = CK_NEW;
	osmo_timer_setup(&c->timer, conn_timer_cb, c);
	if (aworker_ipc_link_alloc(&c->link, g_s.ctx, fd, &cfg, &conn_ops, c) < 0) {
		close(fd);
		talloc_free(c);
		return 0;
	}
	llist_add_tail(&c->entry, &g_s.conns);
	g_s.num_conns++;
	osmo_timer_schedule(&c->timer, AWORKER_SESSION_FIRST_RECORD_S, 0);
	LOGS(LOGL_INFO, "connection of pid %ld (uid %u) accepted\n", (long)cr.pid, (unsigned int)cr.uid);
	return 0;
}

/***********************************************************************
 * The socket (N5)
 ***********************************************************************/

static int refuse(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int refuse(const char *fmt, ...)
{
	char why[400];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s: supervisor-socket: %s\n", aworker_program_name, why);
	LOGS(LOGL_FATAL, "supervisor-socket: %s\n", why);
	return -EINVAL;
}

int aworker_session_start(void *ctx, const char *path, const char *doc, size_t doc_len,
			  const struct aworker_session_test_hooks *th)
{
	struct sockaddr_un sun = { .sun_family = AF_UNIX };
	char *dir_copy;
	struct stat st;
	mode_t old;
	int fd, rc;
	unsigned int i;

	OSMO_ASSERT(!g_s.started);
	g_s.ctx = talloc_named_const(ctx, 0, "aworker-session");
	INIT_LLIST_HEAD(&g_s.conns);
	g_s.th = th ? *th : (struct aworker_session_test_hooks){ .fence_signal = -1, .peer_uid = -1 };
	g_s.lock_fd = -1;
	g_s.pidfd = -1;
	g_s.pidofd.fd = -1;
	g_s.lfd.fd = -1;
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		osmo_timer_setup(&g_s.w[i].retry, retry_cb, &g_s.w[i]);
		osmo_timer_setup(&g_s.w[i].ttimer, ttimer_cb, &g_s.w[i]);
	}
	osmo_timer_setup(&g_s.echo_timer, echo_timer_cb, NULL);
	osmo_timer_setup(&g_s.fence_timer, fence_timer_cb, NULL);

	if (!path || path[0] != '/' || strlen(path) >= sizeof(sun.sun_path))
		return refuse("'%s' is not an absolute path shorter than %zu characters", path ? path : "",
			      sizeof(sun.sun_path));
	g_s.path = talloc_strdup(g_s.ctx, path);
	g_s.lock_path = talloc_asprintf(g_s.ctx, "%s.lock", path);
	g_s.doc = talloc_memdup(g_s.ctx, doc, doc_len);
	g_s.doc_len = doc_len;
	if (!g_s.path || !g_s.lock_path || !g_s.doc)
		return -ENOMEM;

	/* The directory: exists, is one, ours, writable by nobody else. */
	dir_copy = talloc_strdup(g_s.ctx, path);
	if (!dir_copy)
		return -ENOMEM;
	rc = lstat(dirname(dir_copy), &st);
	if (rc < 0)
		return refuse("its directory %s: %s (it is not created)", dir_copy, strerror(errno));
	if (!S_ISDIR(st.st_mode))
		return refuse("%s is not a directory", dir_copy);
	if (st.st_uid != geteuid())
		return refuse("its directory %s belongs to uid %u, not to this process' uid %u", dir_copy,
			      (unsigned int)st.st_uid, (unsigned int)geteuid());
	if (st.st_mode & 022)
		return refuse("its directory %s is writable by others (mode %03o)", dir_copy,
			      (unsigned int)(st.st_mode & 0777));
	talloc_free(dir_copy);

	/* The lock, held for our lifetime. */
	fd = open(g_s.lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return refuse("cannot open the lock file %s: %s", g_s.lock_path, strerror(errno));
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) {
		close(fd);
		return refuse("the lock file %s is not a regular file of this user with mode 0600", g_s.lock_path);
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		rc = errno;
		close(fd);
		if (rc == EWOULDBLOCK)
			return refuse("%s is held by another process: another Supervisor uses this socket", g_s.lock_path);
		return refuse("cannot lock %s: %s", g_s.lock_path, strerror(rc));
	}
	g_s.lock_fd = fd;

	/* Holding the lock: a leftover socket is ours to remove; anything else at the path is not touched. */
	if (lstat(path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			aworker_session_stop();
			return refuse("%s exists and is not a socket; not removing it", path);
		}
		if (unlink(path) < 0) {
			rc = errno;
			aworker_session_stop();
			return refuse("cannot remove the leftover socket %s: %s", path, strerror(rc));
		}
		LOGS(LOGL_NOTICE, "removed the leftover socket %s (holding %s)\n", path, g_s.lock_path);
	}

	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		rc = errno;
		aworker_session_stop();
		return refuse("socket(): %s", strerror(rc));
	}
	OSMO_STRLCPY_ARRAY(sun.sun_path, path);
	old = umask(0177);
	rc = bind(fd, (struct sockaddr *)&sun, sizeof(sun));
	umask(old);
	if (rc < 0 || chmod(path, 0600) < 0 || listen(fd, AWORKER_SESSION_MAX_CONNS) < 0) {
		rc = errno;
		close(fd);
		aworker_session_stop();
		return refuse("cannot listen on %s: %s", path, strerror(rc));
	}
	osmo_fd_setup(&g_s.lfd, fd, OSMO_FD_READ, listen_cb, NULL, 0);
	if (osmo_fd_register(&g_s.lfd) < 0) {
		close(fd);
		g_s.lfd.fd = -1;
		aworker_session_stop();
		return refuse("cannot register the socket");
	}
	g_s.started = true;
	g_s.state = S_NONE;
	aworker_pool_set_session_hooks(&session_hooks);
	LOGS(LOGL_NOTICE, "listening on %s (mode 0600; peers: uid %u or root)\n", path,
	     (unsigned int)(g_s.th.peer_uid >= 0 ? (uid_t)g_s.th.peer_uid : geteuid()));
	return 0;
}

static bool all_written(void)
{
	struct conn *c;

	llist_for_each_entry(c, &g_s.conns, entry) {
		struct aworker_ipc_link_stats st;
		aworker_ipc_link_get_stats(c->link, &st);
		if (st.state != AWORKER_IPC_LINK_CLOSED && st.last_written_seq != st.last_queued_seq)
			return false;
	}
	return true;
}

void aworker_session_stop(void)
{
	struct conn *c, *c2;
	struct timespec t0, t;
	unsigned int i;

	if (!g_s.ctx)
		return;	/* no supervisor-socket: nothing was set up */
	g_s.stopping = true;
	if (g_s.conn && session_live()) {
		struct msgb *msg;
		if (aworker_ipc_enc_session_close(g_s.ctx, &msg, AWORKER_IPC_CC_SHUTDOWN) == 0)
			conn_send(g_s.conn, msg, -1, NULL);
	}
	/* Let the answers and the SESSION_CLOSE go out (bounded). */
	osmo_clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!all_written()) {
		osmo_clock_gettime(CLOCK_MONOTONIC, &t);
		if ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 > FLUSH_MS)
			break;
		osmo_select_main(1);
		usleep(1000);
	}
	if (session_live())
		session_close("the pool is shutting down");
	llist_for_each_entry_safe(c, c2, &g_s.conns, entry)
		conn_free(c);
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		osmo_timer_del(&g_s.w[i].retry);
		osmo_timer_del(&g_s.w[i].ttimer);
	}
	osmo_timer_del(&g_s.echo_timer);
	osmo_timer_del(&g_s.fence_timer);
	pidfd_release();
	aworker_pool_set_session_hooks(NULL);
	if (g_s.lfd.fd >= 0) {
		osmo_fd_unregister(&g_s.lfd);
		close(g_s.lfd.fd);
		g_s.lfd.fd = -1;
		/* Ours to remove: we still hold the lock. */
		unlink(g_s.path);
	}
	if (g_s.lock_fd >= 0) {
		close(g_s.lock_fd);	/* releases the lock; the lock file stays */
		g_s.lock_fd = -1;
	}
	if (g_s.started)
		LOGS(LOGL_NOTICE, "socket %s closed\n", g_s.path);
	g_s.started = false;
}

unsigned int aworker_session_closing_entries(void)
{
	return g_s.closing_entries;
}

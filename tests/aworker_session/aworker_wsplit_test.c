/* A bsc-aworker worker with split links (S2-5 stage 3): control link, ATTACH/DETACH, data link, minimal detach */
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

/* W: the real executable (argv[1], bsc-aworker) runs its hidden worker role, started by this process exactly as the
 * Supervisor starts it (--aworker-fd, --aworker-index, --aworker-snapshot-fd, --aworker-incarnation). This process plays
 * the Supervisor on the control link and the BSC on the data links it hands over with ATTACH. The worker's asp points
 * at a free local port nobody listens on, so it never reaches a real STP. */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/aworker_snapshot.h>

#define NONCE	0x5a5a00001234abcdULL
#define INC	3

static void *ctx;
static unsigned int failures;
static const char *g_bin;
static const char *g_osmo_bsc;	/* optional: osmo-bsc, for the legacy worker (W11) */
static char g_dir[256];

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			fflush(stdout); \
			failures++; \
		} \
	} while (0)

static const struct log_info test_log_info = {};

static unsigned int ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

/* Run the event loop until cond holds or ms passed. */
#define WAIT_FOR(cond, ms) do { \
		struct timespec _t0; \
		clock_gettime(CLOCK_MONOTONIC, &_t0); \
		while (!(cond) && ms_since(&_t0) < (ms)) { \
			osmo_select_main(1); \
			usleep(1000); \
		} \
	} while (0)

static unsigned int free_port(void)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t l = sizeof(a);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) || getsockname(fd, (struct sockaddr *)&a, &l))
		abort();
	close(fd);
	return ntohs(a.sin_port);
}

static unsigned int fds_of(pid_t pid)
{
	char p[64];
	struct dirent *de;
	unsigned int n = 0;
	DIR *d;

	snprintf(p, sizeof(p), "/proc/%ld/fd", (long)pid);
	d = opendir(p);
	if (!d)
		return 0;
	while ((de = readdir(d)))
		n += de->d_name[0] != '.';
	closedir(d);
	return n;
}

/***********************************************************************
 * One end of a link, recording what arrives
 ***********************************************************************/

struct end {
	const char *name;
	struct aworker_ipc_link *link;
	unsigned int ready, closed, rx;
	struct aworker_ipc_link_close_info ci;
	struct aworker_ipc_msg last;
	unsigned int n_state;
	struct aworker_ipc_msg state;	/* the last STATE */
};

static void e_ready(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	struct end *e = aworker_ipc_link_get_priv(link);
	e->ready++;
}

static void e_rx(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	struct end *e = aworker_ipc_link_get_priv(link);
	e->rx++;
	e->last = *m;
	if (m->type == AWORKER_IPC_MT_STATE) {
		e->n_state++;
		e->state = *m;
	}
}

static void e_closed(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	struct end *e = aworker_ipc_link_get_priv(link);
	e->closed++;
	e->ci = *info;
}

static const struct aworker_ipc_link_ops e_ops = { .ready_cb = e_ready, .rx_cb = e_rx, .closed_cb = e_closed };

static void end_free(struct end *e)
{
	if (e->link)
		aworker_ipc_link_free(e->link);
	e->link = NULL;
}

/* Our Supervisor end of the control link (classes as given); with_id false: assert no identity (osmo-bsc's main
 * process does not). */
static void sup_alloc(struct end *e, int fd, unsigned int classes, bool with_id)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_MAIN, .worker_idx = 0, .pid = (uint32_t)getpid(), .ident = "test-supervisor",
		.log_cat = DLGLOBAL, .classes = classes,
	};
	if (with_id)
		cfg.id = (struct aworker_ipc_identity){ .boot_nonce = NONCE, .incarnation = INC };
	memset(e, 0, sizeof(*e));
	e->name = "supervisor";
	OSMO_ASSERT(aworker_ipc_link_alloc(&e->link, ctx, fd, &cfg, &e_ops, e) == 0);
}

/* Our BSC end of a data link, asserting session (and the worker's incarnation and the boot nonce). */
static void bsc_alloc(struct end *e, int fd, uint32_t session)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_MAIN, .worker_idx = 0, .pid = (uint32_t)getpid(), .ident = "test-bsc",
		.log_cat = DLGLOBAL, .classes = AWORKER_IPC_CLASS_DATA,
		.id = { .boot_nonce = NONCE, .incarnation = INC, .session_id = session },
	};
	memset(e, 0, sizeof(*e));
	e->name = "bsc";
	OSMO_ASSERT(aworker_ipc_link_alloc(&e->link, ctx, fd, &cfg, &e_ops, e) == 0);
}

/***********************************************************************
 * The worker process
 ***********************************************************************/

struct worker {
	pid_t pid;
	struct end sup;
	struct aworker_snapshot *snap;
	char log[300];
};

/* bin: the executable to run in its worker role; incarnation 0: no --aworker-incarnation (osmo-bsc's workers). */
static void worker_start_bin(struct worker *w, const char *bin, unsigned int incarnation, unsigned int sup_classes)
{
	char text[2048], fdarg[16], snaparg[16], incarg[16];
	int sv[2], logfd;
	unsigned int port = free_port();

	snprintf(text, sizeof(text),
		 "log stderr\n"
		 " logging filter all 1\n"
		 " logging color 0\n"
		 " logging print category 1\n"
		 " logging print file 0\n"
		 " logging level set-all notice\n"
		 " logging level aworker info\n"
		 "cs7 instance 0\n"
		 " point-code 0.23.3\n"
		 " asp asp-w0 %u 0 m3ua tcp\n"
		 "  remote-ip 127.0.0.1\n"
		 "  local-ip 127.0.0.1\n"
		 "  role asp\n"
		 "  transport-role client\n"
		 "a-worker-pool\n"
		 " workers 1\n"
		 " worker 0 cs7-instance 0\n", port);
	OSMO_ASSERT(aworker_snapshot_create(ctx, 0, text, strlen(text), &w->snap) == 0);
	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
	snprintf(w->log, sizeof(w->log), "%s/worker.%u.log", g_dir, incarnation);

	w->pid = fork();
	OSMO_ASSERT(w->pid >= 0);
	if (w->pid == 0) {
		/* the worker's descriptors: stdio, its control socket, its snapshot -- nothing else */
		logfd = open(w->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (logfd < 0 || dup2(logfd, 2) < 0 || dup2(sv[1], 10) < 0 || dup2(w->snap->fd, 11) < 0)
			_exit(99);
		close_range(12, ~0U, 0);
		for (int fd = 3; fd < 10; fd++)
			close(fd);
		snprintf(fdarg, sizeof(fdarg), "10");
		snprintf(snaparg, sizeof(snaparg), "11");
		snprintf(incarg, sizeof(incarg), "%u", incarnation);
		if (incarnation)
			execl(bin, bin, "--aworker-fd", fdarg, "--aworker-index", "0", "--aworker-snapshot-fd", snaparg,
			      "--aworker-incarnation", incarg, (char *)NULL);
		else
			execl(bin, bin, "--aworker-fd", fdarg, "--aworker-index", "0", "--aworker-snapshot-fd", snaparg,
			      (char *)NULL);
		_exit(98);
	}
	close(sv[1]);
	sup_alloc(&w->sup, sv[0], sup_classes, incarnation != 0);
}

static void worker_start(struct worker *w, unsigned int incarnation, unsigned int sup_classes)
{
	worker_start_bin(w, g_bin, incarnation, sup_classes);
}

/* \returns the exit status (-1: not exited within ms). */
static int worker_wait(struct worker *w, unsigned int ms)
{
	int status = 0;
	pid_t r = 0;

	WAIT_FOR((r = waitpid(w->pid, &status, WNOHANG)) == w->pid, ms);
	if (r != w->pid) {
		kill(w->pid, SIGKILL);
		waitpid(w->pid, &status, 0);
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void worker_end(struct worker *w)
{
	end_free(&w->sup);
	aworker_snapshot_close(w->snap);
	w->snap = NULL;
}

static bool log_has(const struct worker *w, const char *text)
{
	char buf[65536];
	int fd = open(w->log, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return false;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return false;
	buf[n] = '\0';
	return strstr(buf, text) != NULL;
}

/* ATTACH(session, incarnation) with a new socketpair; *bsc_fd gets our (BSC) end. \returns the STATE answered. */
static const struct aworker_ipc_msg *attach(struct worker *w, uint32_t session, uint32_t incarnation, int *bsc_fd)
{
	unsigned int n = w->sup.n_state;
	struct msgb *msg;
	int sp[2];

	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0);
	OSMO_ASSERT(aworker_ipc_enc_attach(ctx, &msg, session, incarnation) == 0);
	CHECK(aworker_ipc_link_send_fd(w->sup.link, msg, sp[1], NULL) == 0);
	*bsc_fd = sp[0];
	WAIT_FOR(w->sup.n_state > n, 5000);
	CHECK(w->sup.n_state == n + 1);
	return &w->sup.state;
}

static const struct aworker_ipc_msg *detach_req(struct worker *w, uint32_t session)
{
	unsigned int n = w->sup.n_state;
	struct msgb *msg;

	OSMO_ASSERT(aworker_ipc_enc_detach(ctx, &msg, session) == 0);
	CHECK(aworker_ipc_link_send(w->sup.link, msg, NULL) == 0);
	WAIT_FOR(w->sup.n_state > n, 5000);
	CHECK(w->sup.n_state == n + 1);
	return &w->sup.state;
}

static void print_state(const char *what, const struct aworker_ipc_msg *s)
{
	printf("  %s: STATE session %u %s %s, users unbound %u\n", what, s->u.state.session_id,
	       aworker_ipc_attach_state_name(s->u.state.state), aworker_ipc_state_cause_name(s->u.state.cause),
	       s->u.state.users_unbound);
}

/* The other end of a socketpair sees EOF (nobody holds the peer end any more) within ms. */
static bool eof_within(int fd, unsigned int ms)
{
	struct timespec t0;
	char b;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	while (ms_since(&t0) < ms) {
		ssize_t r = recv(fd, &b, 1, 0);
		if (r == 0)
			return true;
		osmo_select_main(1);
		usleep(2000);
	}
	return false;
}

static void test_w(void)
{
	const struct aworker_ipc_msg *s;
	struct worker w = {};
	struct end bsc;
	struct msgb *msg;
	unsigned int base_fds;
	int fd, rc;

	printf("W1: the worker's control link (CONTROL only) and CS7_INFO\n");
	worker_start(&w, INC, AWORKER_IPC_CLASS_CONTROL);
	WAIT_FOR(w.sup.last.type == AWORKER_IPC_MT_CS7_INFO || w.sup.closed, 10000);
	CHECK(w.sup.ready == 1 && w.sup.last.type == AWORKER_IPC_MT_CS7_INFO);
	printf("  handshake %s, CS7_INFO for cs7 instance %u\n", w.sup.ready ? "complete" : "MISSING",
	       w.sup.last.u.cs7_info.e[0].cs7_id);
	usleep(200000);
	base_fds = fds_of(w.pid);

	printf("W2: ATTACH, the data link, an SCCP user over it\n");
	s = attach(&w, 5, INC, &fd);
	print_state("ATTACH(5)", s);
	CHECK(s->u.state.session_id == 5 && s->u.state.state == AWORKER_IPC_ATT_ATTACHED
	      && s->u.state.cause == AWORKER_IPC_SC_ATTACH_OK);
	bsc_alloc(&bsc, fd, 5);
	WAIT_FOR(bsc.ready, 5000);
	CHECK(bsc.ready == 1);
	OSMO_ASSERT(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 254, "msc-0") == 0);
	CHECK(aworker_ipc_link_send(bsc.link, msg, NULL) == 0);
	WAIT_FOR(bsc.rx, 5000);
	printf("  USER_BIND over the data link: %s %u\n", aworker_ipc_msg_type_name(bsc.last.type), bsc.last.u.bind_result);
	CHECK(bsc.last.type == AWORKER_IPC_MT_USER_BIND_RESULT && bsc.last.u.bind_result == AWORKER_IPC_BIND_OK);
	CHECK(fds_of(w.pid) == base_fds + 1);

	printf("W3: ATTACH while attached is refused; the descriptor is closed unused\n");
	s = attach(&w, 6, INC, &fd);
	print_state("ATTACH(6)", s);
	CHECK(s->u.state.session_id == 6 && s->u.state.state == AWORKER_IPC_ATT_ATTACHED
	      && s->u.state.cause == AWORKER_IPC_SC_ATTACH_REFUSED);
	CHECK(eof_within(fd, 2000));
	close(fd);
	CHECK(fds_of(w.pid) == base_fds + 1);

	printf("W4: DETACH: users unbound, the data link closed (EOF at the BSC end)\n");
	s = detach_req(&w, 5);
	print_state("DETACH(5)", s);
	CHECK(s->u.state.session_id == 5 && s->u.state.state == AWORKER_IPC_ATT_DETACHED
	      && s->u.state.cause == AWORKER_IPC_SC_DETACH_REQ && s->u.state.users_unbound == 1);
	WAIT_FOR(bsc.closed, 5000);
	printf("  BSC end: %s\n", bsc.closed ? aworker_ipc_close_reason_name(bsc.ci.reason) : "NOT closed");
	CHECK(bsc.closed == 1 && bsc.ci.reason == AWORKER_IPC_CLOSE_PEER_EOF);
	end_free(&bsc);
	CHECK(fds_of(w.pid) == base_fds);

	printf("W5: DETACH again: answered the same way (idempotent)\n");
	s = detach_req(&w, 5);
	print_state("DETACH(5) again", s);
	CHECK(s->u.state.state == AWORKER_IPC_ATT_DETACHED && s->u.state.cause == AWORKER_IPC_SC_DETACH_REQ
	      && s->u.state.users_unbound == 0);

	printf("W6: ATTACH for another incarnation is refused; the descriptor is closed unused\n");
	s = attach(&w, 7, INC + 1, &fd);
	print_state("ATTACH(7, incarnation 4)", s);
	CHECK(s->u.state.state == AWORKER_IPC_ATT_DETACHED && s->u.state.cause == AWORKER_IPC_SC_ATTACH_REFUSED);
	CHECK(eof_within(fd, 2000));
	close(fd);

	printf("W7: the BSC end goes away: the worker sees EOF itself and detaches\n");
	s = attach(&w, 8, INC, &fd);
	CHECK(s->u.state.cause == AWORKER_IPC_SC_ATTACH_OK);
	bsc_alloc(&bsc, fd, 8);
	WAIT_FOR(bsc.ready, 5000);
	OSMO_ASSERT(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 254, "msc-0") == 0);
	CHECK(aworker_ipc_link_send(bsc.link, msg, NULL) == 0);
	WAIT_FOR(bsc.rx, 5000);
	{
		unsigned int n = w.sup.n_state;
		end_free(&bsc);		/* closes our end: the worker's is the last one */
		WAIT_FOR(w.sup.n_state > n, 5000);
		CHECK(w.sup.n_state == n + 1);
	}
	print_state("unsolicited", &w.sup.state);
	CHECK(w.sup.state.u.state.session_id == 8 && w.sup.state.u.state.state == AWORKER_IPC_ATT_DETACHED
	      && w.sup.state.u.state.cause == AWORKER_IPC_SC_DATA_EOF && w.sup.state.u.state.users_unbound == 1);
	CHECK(fds_of(w.pid) == base_fds);

	printf("W8: a BSC end of another session fails the data handshake: detached\n");
	s = attach(&w, 9, INC, &fd);
	CHECK(s->u.state.cause == AWORKER_IPC_SC_ATTACH_OK);
	{
		/* A raw BSC end that only sends its HELLO (for session 10) and keeps the socket open, so that the
		 * worker's own check is what ends the data link (not our EOF). */
		struct aworker_ipc_identity id = { .boot_nonce = NONCE, .incarnation = INC, .session_id = 10 };
		unsigned int n = w.sup.n_state;
		OSMO_ASSERT(aworker_ipc_enc_hello_id(ctx, &msg, AWORKER_IPC_ROLE_MAIN, 0, getpid(), "raw-bsc", &id) == 0);
		aworker_ipc_frame_set_seq(msgb_data(msg), 1);
		CHECK(send(fd, msgb_data(msg), msgb_length(msg), 0) == (ssize_t)msgb_length(msg));
		msgb_free(msg);
		WAIT_FOR(w.sup.n_state > n, 5000);
		CHECK(w.sup.n_state == n + 1);
	}
	print_state("data HELLO for session 10", &w.sup.state);
	CHECK(w.sup.state.u.state.session_id == 9 && w.sup.state.u.state.cause == AWORKER_IPC_SC_DATA_ERROR);
	CHECK(eof_within(fd, 2000));
	close(fd);
	CHECK(fds_of(w.pid) == base_fds);

	printf("W8b: a BSC end of another Supervisor boot (right session) fails the data handshake as well\n");
	s = attach(&w, 11, INC, &fd);
	CHECK(s->u.state.cause == AWORKER_IPC_SC_ATTACH_OK);
	{
		struct aworker_ipc_identity id = { .boot_nonce = NONCE + 1, .incarnation = INC, .session_id = 11 };
		unsigned int n = w.sup.n_state;
		OSMO_ASSERT(aworker_ipc_enc_hello_id(ctx, &msg, AWORKER_IPC_ROLE_MAIN, 0, getpid(), "raw-bsc", &id) == 0);
		aworker_ipc_frame_set_seq(msgb_data(msg), 1);
		CHECK(send(fd, msgb_data(msg), msgb_length(msg), 0) == (ssize_t)msgb_length(msg));
		msgb_free(msg);
		WAIT_FOR(w.sup.n_state > n, 5000);
		CHECK(w.sup.n_state == n + 1);
	}
	print_state("data HELLO of another boot", &w.sup.state);
	CHECK(w.sup.state.u.state.session_id == 11 && w.sup.state.u.state.cause == AWORKER_IPC_SC_DATA_ERROR);
	CHECK(eof_within(fd, 2000));
	close(fd);
	CHECK(fds_of(w.pid) == base_fds);
	CHECK(!w.sup.closed);

	printf("W9: GOODBYE ends the worker\n");
	OSMO_ASSERT(aworker_ipc_enc_goodbye(ctx, &msg, AWORKER_IPC_GOODBYE_SHUTDOWN) == 0);
	CHECK(aworker_ipc_link_send(w.sup.link, msg, NULL) == 0);
	rc = worker_wait(&w, 10000);
	printf("  exit %d\n", rc);
	CHECK(rc == AWORKER_EXIT_OK);
	CHECK(log_has(&w, "detached from session 5 (detach-req): 1 SCCP user(s) unbound; SS7 and SCTP unchanged"));
	CHECK(log_has(&w, "data link of session 8 closed: peer-eof (0): the BSC end is gone"));
	worker_end(&w);

	printf("W10: an SCCP-user record on the control link closes it: the worker exits\n");
	worker_start(&w, INC, AWORKER_IPC_CLASS_CONTROL | AWORKER_IPC_CLASS_DATA);
	WAIT_FOR(w.sup.last.type == AWORKER_IPC_MT_CS7_INFO || w.sup.closed, 10000);
	OSMO_ASSERT(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 254, "msc-0") == 0);
	CHECK(aworker_ipc_link_send(w.sup.link, msg, NULL) == 0);
	rc = worker_wait(&w, 10000);
	printf("  exit %d\n", rc);
	CHECK(rc == AWORKER_EXIT_LINK);
	CHECK(log_has(&w, "which this link does not carry"));
	worker_end(&w);

	if (!g_osmo_bsc)
		return;
	printf("W11: osmo-bsc's worker (legacy): one link for control and SCCP users, as before; no ATTACH\n");
	worker_start_bin(&w, g_osmo_bsc, 0, 0);
	WAIT_FOR(w.sup.last.type == AWORKER_IPC_MT_CS7_INFO || w.sup.closed, 10000);
	CHECK(w.sup.last.type == AWORKER_IPC_MT_CS7_INFO);
	OSMO_ASSERT(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 254, "msc-0") == 0);
	CHECK(aworker_ipc_link_send(w.sup.link, msg, NULL) == 0);
	WAIT_FOR(w.sup.last.type == AWORKER_IPC_MT_USER_BIND_RESULT || w.sup.closed, 5000);
	printf("  USER_BIND on the one link: %s %u\n", aworker_ipc_msg_type_name(w.sup.last.type),
	       w.sup.last.u.bind_result);
	CHECK(w.sup.last.type == AWORKER_IPC_MT_USER_BIND_RESULT && w.sup.last.u.bind_result == AWORKER_IPC_BIND_OK);
	{
		int sp[2];
		OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0);
		OSMO_ASSERT(aworker_ipc_enc_attach(ctx, &msg, 5, 1) == 0);
		CHECK(aworker_ipc_link_send_fd(w.sup.link, msg, sp[1], NULL) == 0);
		close(sp[0]);
	}
	rc = worker_wait(&w, 10000);
	printf("  ATTACH to a legacy worker: exit %d\n", rc);
	CHECK(rc == AWORKER_EXIT_LINK);
	worker_end(&w);
}

int main(int argc, char **argv)
{
	char cmd[300];

	if (argc < 2) {
		fprintf(stderr, "usage: %s PATH-TO-bsc-aworker [PATH-TO-osmo-bsc]\n", argv[0]);
		return 2;
	}
	g_bin = argv[1];
	g_osmo_bsc = argc > 2 ? argv[2] : NULL;
	ctx = talloc_named_const(NULL, 0, "aworker_wsplit_test");
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_log_level(osmo_stderr_target, LOGL_FATAL);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(g_dir, sizeof(g_dir), "%s/aworker_wsplit.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));

	test_w();

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("worker logs kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

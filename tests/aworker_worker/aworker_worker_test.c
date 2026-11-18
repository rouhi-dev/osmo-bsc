/* Tests of the A-worker's snapshot mode and its command line option (S2-2 step 6, decision D-6) */
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

/* W: the real aworker_run(), in a child process of its own, with this process as its main process on the IPC link.
 *    What the child does is recorded through test-only wraps (-Wl,--wrap, see Makefile.am): whether the configuration
 *    was parsed at all, whether the snapshot descriptor was already closed then, and what exists afterwards (cs7
 *    instances, ASPs, listening sockets) -- also when it exits (atexit()). Transports use TCP on 127.0.0.1 with ports
 *    free when the test starts; nothing outside this host is contacted.
 * R-1: the real osmo-bsc binary (argv[1], or ../../src/osmo-bsc/osmo-bsc from the test's build directory) with the
 *    worker options: every invalid snapshot-mode combination exits 2; the legacy ones behave as before. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker.h>
#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/aworker_snapshot.h>

static void *ctx;
static char g_dir[256];
static unsigned int g_port, g_port2;
static unsigned int failures;

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			failures++; \
		} \
	} while (0)

static const struct log_info test_log_info = {
	.cat = osmo_bsc_categories,
	.num_cat = Debug_LastEntry,
};

static struct vty_app_info vty_info = {
	.name = "aworker_worker_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

static unsigned long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static unsigned int free_port(void)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	socklen_t l = sizeof(a);
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	OSMO_ASSERT(fd >= 0);
	OSMO_ASSERT(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	OSMO_ASSERT(getsockname(fd, (struct sockaddr *)&a, &l) == 0);
	close(fd);
	return ntohs(a.sin_port);
}

/* Text with every "@P" replaced by g_port and "@Q" by g_port2 (talloc'ed). */
static char *ports(const char *text)
{
	char *out = talloc_strdup(ctx, "");
	const char *s;

	for (s = text; *s; s++) {
		if (s[0] == '@' && (s[1] == 'P' || s[1] == 'Q')) {
			out = talloc_asprintf_append_buffer(out, "%u", s[1] == 'P' ? g_port : g_port2);
			s++;
		} else {
			out = talloc_strndup_append_buffer(out, s, 1);
		}
		OSMO_ASSERT(out);
	}
	return out;
}

/* Listening TCP sockets of this process. */
static unsigned int count_own_listen_sockets(void)
{
	static const char *const tables[] = { "/proc/self/net/tcp", "/proc/self/net/tcp6" };
	unsigned long inodes[256];
	unsigned int n_inodes = 0, count = 0, t;
	struct dirent *de;
	DIR *d = opendir("/proc/self/fd");

	OSMO_ASSERT(d);
	while ((de = readdir(d)) && n_inodes < ARRAY_SIZE(inodes)) {
		char p[300], target[64];
		ssize_t l;
		snprintf(p, sizeof(p), "/proc/self/fd/%s", de->d_name);
		l = readlink(p, target, sizeof(target) - 1);
		if (l <= 0)
			continue;
		target[l] = '\0';
		if (!strncmp(target, "socket:[", 8))
			inodes[n_inodes++] = strtoul(target + 8, NULL, 10);
	}
	closedir(d);
	for (t = 0; t < ARRAY_SIZE(tables); t++) {
		char line[512];
		FILE *f = fopen(tables[t], "r");
		if (!f)
			continue;
		while (fgets(line, sizeof(line), f)) {
			char st[8];
			unsigned long inode;
			unsigned int i;
			if (sscanf(line, " %*s %*s %*s %7s %*s %*s %*s %*s %*s %lu", st, &inode) != 2 || strcmp(st, "0A"))
				continue;
			for (i = 0; i < n_inodes; i++)
				count += inodes[i] == inode;
		}
		fclose(f);
	}
	return count;
}

/***********************************************************************
 * The child's side: what it did, recorded through wraps and atexit()
 ***********************************************************************/

static struct {
	int report_fd;		/* pipe to the parent */
	int snap_fd;		/* the snapshot descriptor the child was given (-1: legacy) */
	bool snap_known;	/* snap_dev / snap_ino: the file it referred to (if it was open) */
	dev_t snap_dev;
	ino_t snap_ino;
	bool bypass_d13;	/* aworker_cfg_check_worker_view() finds nothing */
	unsigned int parses;
} g_child;

static unsigned int cs7_instances(void)
{
	unsigned int n = 0, i;
	for (i = 0; i < AWORKER_CFG_VIEW_MAX_CS7; i++)
		n += osmo_ss7_instance_find(i) != NULL;
	return n;
}

static void child_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void child_report(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > 0 && write(g_child.report_fd, buf, OSMO_MIN((size_t)n, sizeof(buf) - 1)) < 0) {}
}

/* Whether the snapshot descriptor is still open. By its file, not its number: once it is closed, its number may be
 * reused by any socket the worker opens. */
static const char *snap_state(void)
{
	struct stat st;

	if (g_child.snap_fd < 0)
		return "none";
	if (fstat(g_child.snap_fd, &st) < 0)
		return "closed";
	return g_child.snap_known && st.st_dev == g_child.snap_dev && st.st_ino == g_child.snap_ino ? "OPEN" : "closed";
}

int __real_vty_read_config_filep(FILE *confp, void *priv);
int __wrap_vty_read_config_filep(FILE *confp, void *priv);
int __wrap_vty_read_config_filep(FILE *confp, void *priv)
{
	int rc;

	g_child.parses++;
	child_report("parse started; snapshot descriptor %s then\n", snap_state());
	rc = __real_vty_read_config_filep(confp, priv);
	child_report("parse result %d; afterwards: cs7 instances %u, listening sockets %u\n", rc, cs7_instances(),
		     count_own_listen_sockets());
	return rc;
}

unsigned int __real_aworker_cfg_check_worker_view(const char *text, size_t len, unsigned int worker_idx,
						  struct aworker_cfg_problem *out, unsigned int max);
unsigned int __wrap_aworker_cfg_check_worker_view(const char *text, size_t len, unsigned int worker_idx,
						  struct aworker_cfg_problem *out, unsigned int max);
unsigned int __wrap_aworker_cfg_check_worker_view(const char *text, size_t len, unsigned int worker_idx,
						  struct aworker_cfg_problem *out, unsigned int max)
{
	unsigned int n = __real_aworker_cfg_check_worker_view(text, len, worker_idx, out, max);
	child_report("view check: %u problem(s)%s\n", n, g_child.bypass_d13 ? ", ignored (test bypass)" : "");
	return g_child.bypass_d13 ? 0 : n;
}

static void child_atexit(void)
{
	child_report("exit: parses %u; snapshot descriptor %s; cs7 instances %u; listening sockets %u\n",
		     g_child.parses, snap_state(), cs7_instances(), count_own_listen_sockets());
}

/***********************************************************************
 * The parent's side: the main process on the IPC link
 ***********************************************************************/

static struct {
	bool cs7_info;
	uint32_t cs7_id[4];
	unsigned int cs7_count;
	bool closed;
} g_main;

static void m_ready_cb(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
}

static void m_rx_cb(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	unsigned int i;

	if (m->type != AWORKER_IPC_MT_CS7_INFO)
		return;
	g_main.cs7_info = true;
	g_main.cs7_count = m->u.cs7_info.count;
	for (i = 0; i < m->u.cs7_info.count && i < ARRAY_SIZE(g_main.cs7_id); i++)
		g_main.cs7_id[i] = m->u.cs7_info.e[i].cs7_id;
}

static void m_closed_cb(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	g_main.closed = true;
}

static const struct aworker_ipc_link_ops m_ops = {
	.ready_cb = m_ready_cb,
	.rx_cb = m_rx_cb,
	.closed_cb = m_closed_cb,
};

struct w_case {
	const char *label;
	unsigned int idx;
	const char *text;	/* the snapshot's contents (snapshot mode) */
	bool unsealed;		/* a memfd without seals instead of a snapshot */
	bool closed_fd;		/* a descriptor that is not open */
	const char *legacy_file;/* legacy mode: this config file */
	bool bypass_d13;
	unsigned int incarnation;	/* the worker's (aworker_incarnation; 0: unknown) */
	unsigned int main_incarnation;	/* what the main process' HELLO asserts (0: nothing) */
};

/* What the child reported, and how it ended. */
struct w_result {
	char report[4096];
	char log[16384];
	int exit_code;		/* -1: did not exit (killed) */
	bool cs7_info;
	unsigned int cs7_count;
	uint32_t cs7_id;
};

static void slurp_fd(int fd, char *out, size_t len)
{
	ssize_t n, got = 0;

	while (got < (ssize_t)len - 1 && (n = read(fd, out + got, len - 1 - got)) > 0)
		got += n;
	out[got] = '\0';
}

static void run_worker(const struct w_case *c, struct w_result *r)
{
	struct aworker_snapshot *s = NULL;
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_MAIN,
		.worker_idx = (uint16_t)c->idx,
		.ident = "aworker_worker_test",
		.log_cat = DAWORKER,
		.id = { .incarnation = c->main_incarnation },
	};
	struct aworker_ipc_link *link = NULL;
	char logpath[300];
	int sp[2], rp[2], snap_fd = -1, status, logfd;
	unsigned long end;
	bool goodbye_sent = false;
	pid_t pid;

	memset(r, 0, sizeof(*r));
	memset(&g_main, 0, sizeof(g_main));
	if (c->legacy_file) {
		snap_fd = -1;
	} else if (c->closed_fd) {
		snap_fd = dup(0);
		close(snap_fd);
	} else if (c->unsealed) {
		snap_fd = memfd_create("unsealed", MFD_CLOEXEC);
		OSMO_ASSERT(snap_fd >= 0 && write(snap_fd, c->text, strlen(c->text)) == (ssize_t)strlen(c->text));
	} else {
		OSMO_ASSERT(aworker_snapshot_create(ctx, c->idx, c->text, strlen(c->text), &s) == 0);
		snap_fd = s->fd;
	}
	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sp) == 0);
	OSMO_ASSERT(pipe2(rp, O_CLOEXEC) == 0);
	snprintf(logpath, sizeof(logpath), "%s/worker.log", g_dir);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		close(sp[0]);
		close(rp[0]);
		/* The worker's log goes to a file the parent reads afterwards. */
		logfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		OSMO_ASSERT(logfd >= 0 && dup2(logfd, 2) == 2);
		close(logfd);
		g_child.report_fd = rp[1];
		g_child.snap_fd = snap_fd;
		{
			struct stat st;
			g_child.snap_known = snap_fd >= 0 && fstat(snap_fd, &st) == 0;
			if (g_child.snap_known) {
				g_child.snap_dev = st.st_dev;
				g_child.snap_ino = st.st_ino;
			}
		}
		g_child.bypass_d13 = c->bypass_d13;
		atexit(child_atexit);
		aworker_incarnation = c->incarnation;
		aworker_run(ctx, sp[1], c->idx, c->legacy_file, snap_fd);
	}
	close(sp[1]);
	close(rp[1]);
	OSMO_ASSERT(aworker_ipc_link_alloc(&link, ctx, sp[0], &cfg, &m_ops, NULL) == 0);
	/* Until the worker reported its cs7 instances (then: GOODBYE) or ended; real time, bounded. */
	end = now_ms() + 15000;
	for (;;) {
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid)
			break;
		if (now_ms() > end) {
			kill(pid, SIGKILL);
			OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
			break;
		}
		if (g_main.cs7_info && !goodbye_sent && !g_main.closed) {
			struct msgb *msg;
			OSMO_ASSERT(aworker_ipc_enc_goodbye(ctx, &msg, AWORKER_IPC_GOODBYE_SHUTDOWN) == 0);
			aworker_ipc_link_send(link, msg, NULL);
			goodbye_sent = true;
		}
		osmo_select_main_ctx(1);
		usleep(1000);
	}
	r->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	r->cs7_info = g_main.cs7_info;
	r->cs7_count = g_main.cs7_count;
	r->cs7_id = g_main.cs7_id[0];
	slurp_fd(rp[0], r->report, sizeof(r->report));
	close(rp[0]);
	logfd = open(logpath, O_RDONLY | O_CLOEXEC);
	if (logfd >= 0) {
		slurp_fd(logfd, r->log, sizeof(r->log));
		close(logfd);
	}
	aworker_ipc_link_free(link);
	if (c->unsealed)
		close(snap_fd);
	aworker_snapshot_close(s);
}

/* Run case c; print what happened (the child's report lines, the exit code, the main process' view). */
static void w_case(const struct w_case *c, struct w_result *r)
{
	char *line, *save = NULL, rep[sizeof(r->report)];

	run_worker(c, r);
	printf("  %s (worker %u):\n", c->label, c->idx);
	osmo_strlcpy(rep, r->report, sizeof(rep));
	for (line = strtok_r(rep, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		printf("    child: %s\n", line);
	if (r->cs7_info)
		printf("    main process: CS7_INFO with %u cs7 instance(s), the first %u\n", r->cs7_count, r->cs7_id);
	else
		printf("    main process: no CS7_INFO\n");
	printf("    exit code %d\n", r->exit_code);
}

/***********************************************************************
 * W: snapshot mode, really parsed
 ***********************************************************************/

static const char *cfg_real =
	"log stderr\n"
	" logging level set-all notice\n"
	"line vty\n"
	" bind 127.0.0.1\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" asp asp-a0 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" listen m3ua @Q tcp\n"
	"  local-ip 127.0.0.1\n"
	" asp asp-a1 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n";

static char *view_of(unsigned int idx)
{
	struct aworker_cfg_model *m;
	char *text = ports(cfg_real), *v;

	OSMO_ASSERT(aworker_cfg_model_build_buf(ctx, "real", text, strlen(text), &m) == 0);
	v = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, idx, NULL);
	OSMO_ASSERT(v);
	talloc_free(m);
	talloc_free(text);
	return v;
}

static bool reported(const struct w_result *r, const char *needle)
{
	return strstr(r->report, needle) != NULL;
}

static bool logged(const struct w_result *r, const char *needle)
{
	return strstr(r->log, needle) != NULL;
}

/* A refused snapshot: exit 66, never parsed, the snapshot descriptor closed, nothing created, the reason logged. */
static void expect_refused(const struct w_result *r, const char *why)
{
	CHECK(r->exit_code == AWORKER_EXIT_CONFIG);
	CHECK(!reported(r, "parse started"));
	CHECK(reported(r, "exit: parses 0; snapshot descriptor closed; cs7 instances 0; listening sockets 0"));
	CHECK(!r->cs7_info);
	CHECK(logged(r, why));
	if (!logged(r, why))
		printf("    (expected in the worker's log: '%s')\n", why);
}

static void part_w(void)
{
	char *w0 = view_of(0), *w1 = view_of(1), *t, path[300];
	struct w_case c;
	struct w_result r;
	FILE *f;

	printf("W: snapshot mode, the real aworker_run() in a child, really parsed\n");

	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view, sealed snapshot";
	c.idx = 1;
	c.text = w1;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_OK);
	CHECK(reported(&r, "view check: 0 problem(s)\nparse started; snapshot descriptor closed then\n"));
	CHECK(reported(&r, "parse result 0; afterwards: cs7 instances 1, listening sockets 1"));
	CHECK(r.cs7_info && r.cs7_count == 1 && r.cs7_id == 1);
	CHECK(reported(&r, "exit: parses 1; snapshot descriptor closed;"));

	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view in a memfd without seals";
	c.idx = 1;
	c.text = w1;
	c.unsealed = true;
	w_case(&c, &r);
	expect_refused(&r, "configuration snapshot refused: snapshot descriptor");
	CHECK(!reported(&r, "view check"));

	memset(&c, 0, sizeof(c));
	c.label = "a descriptor that is not open";
	c.idx = 1;
	c.closed_fd = true;
	w_case(&c, &r);
	expect_refused(&r, "configuration snapshot refused: snapshot descriptor");

	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view, given to worker 0";
	c.idx = 0;
	c.text = w1;
	w_case(&c, &r);
	expect_refused(&r, "has no 'cs7 instance 0' section");

	c.label = "the same with the view check bypassed (test only): one cs7 instance, but not the declared one";
	c.bypass_d13 = true;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_CONFIG);
	CHECK(reported(&r, "parse result 0; afterwards: cs7 instances 1,"));
	CHECK(!r.cs7_info);
	CHECK(logged(&r, "are not exactly the cs7 instance 0 declared for this worker"));

	t = talloc_asprintf(ctx, "%s%s", w0, strstr(w1, "cs7 instance 1\n"));
	memset(&c, 0, sizeof(c));
	c.label = "worker 0's view with worker 1's cs7 instance (its listen and asp nodes) added";
	c.idx = 0;
	c.text = t;
	w_case(&c, &r);
	expect_refused(&r, "cs7 instance 1 is not worker 0's");

	c.label = "the same with the view check bypassed (test only): the check after the parse refuses it";
	c.bypass_d13 = true;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_CONFIG);
	CHECK(reported(&r, "ignored (test bypass)\nparse started; snapshot descriptor closed then\n"));
	CHECK(!r.cs7_info);
	CHECK(logged(&r, "are not exactly the cs7 instance 0 declared for this worker"));
	talloc_free(t);

	/* Its own cs7 instance in two sections (each with a transport): with the view check bypassed, the parse creates
	 * one instance, but the configuration names it twice -- the check after the parse refuses it. */
	t = talloc_asprintf(ctx, "%scs7 instance 1\n asp asp-b1 %u 0 m3ua tcp\n  remote-ip 127.0.0.1\n"
			    "  local-ip 127.0.0.1\n  role asp\n  transport-role client\n", w1, g_port);
	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view with its cs7 instance in a second section, view check bypassed (test only)";
	c.idx = 1;
	c.text = t;
	c.bypass_d13 = true;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_CONFIG);
	CHECK(reported(&r, "parse result 0;"));
	CHECK(!r.cs7_info);
	CHECK(logged(&r, "are not exactly the cs7 instance 1 declared for this worker"));
	talloc_free(t);

	snprintf(path, sizeof(path), "%s/worker.file.log", g_dir);
	t = talloc_asprintf(ctx, "%slog file %s\n", w1, path);
	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view with a 'log file'";
	c.idx = 1;
	c.text = t;
	w_case(&c, &r);
	expect_refused(&r, "a worker never writes the BSC's log files");
	printf("    the log file exists: %s\n", access(path, F_OK) == 0 ? "YES" : "no");
	CHECK(access(path, F_OK) != 0);
	talloc_free(t);

	t = talloc_asprintf(ctx, "%s exit\n", w1);
	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view with an 'exit'";
	c.idx = 1;
	c.text = t;
	w_case(&c, &r);
	expect_refused(&r, "'exit' / 'end' in a worker view");
	talloc_free(t);

	/* Legacy mode, unchanged: a config file; the 'a-worker-pool' section is refused there. */
	snprintf(path, sizeof(path), "%s/legacy.cfg", g_dir);
	t = ports("cs7 instance 1\n"
		  " point-code 0.23.4\n"
		  " listen m3ua @Q tcp\n"
		  "  local-ip 127.0.0.1\n"
		  " asp asp-a1 @P 0 m3ua tcp\n"
		  "  remote-ip 127.0.0.1\n"
		  "  local-ip 127.0.0.1\n"
		  "  role asp\n"
		  "  transport-role client\n");
	f = fopen(path, "w");
	OSMO_ASSERT(f && fputs(t, f) >= 0 && fclose(f) == 0);
	memset(&c, 0, sizeof(c));
	c.label = "legacy config file";
	c.idx = 1;
	c.legacy_file = path;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_OK && r.cs7_info && r.cs7_count == 1 && r.cs7_id == 1);
	CHECK(!reported(&r, "view check"));
	/* (A config file is parsed by vty_read_config_file(), inside libosmovty: no "parse" record.) */
	CHECK(reported(&r, "exit: parses 0; snapshot descriptor none; cs7 instances 1; listening sockets 1"));
	talloc_free(t);
	t = talloc_asprintf(ctx, "%sa-worker-pool\n workers 2\n worker 1 cs7-instance 1\n", ports(
		"cs7 instance 1\n"
		" point-code 0.23.4\n"
		" asp asp-a1 @P 0 m3ua tcp\n"
		"  remote-ip 127.0.0.1\n"
		"  local-ip 127.0.0.1\n"
		"  role asp\n"
		"  transport-role client\n"));
	f = fopen(path, "w");
	OSMO_ASSERT(f && fputs(t, f) >= 0 && fclose(f) == 0);
	c.label = "legacy config file with an 'a-worker-pool' section";
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_CONFIG && !r.cs7_info);
	CHECK(logged(&r, "has an 'a-worker-pool' section"));
	talloc_free(t);

	/* Protocol 2.0: a worker that knows its incarnation asserts it in its HELLO; a main process expecting another
	 * one is refused by both sides, before the worker reports anything. */
	memset(&c, 0, sizeof(c));
	c.label = "worker 1's view, incarnation 5, the main process expects 5";
	c.idx = 1;
	c.text = w1;
	c.incarnation = 5;
	c.main_incarnation = 5;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_OK && r.cs7_info && r.cs7_id == 1);
	c.label = "worker 1's view, incarnation 5, the main process expects 6";
	c.main_incarnation = 6;
	w_case(&c, &r);
	CHECK(r.exit_code == AWORKER_EXIT_LINK && !r.cs7_info);
	CHECK(logged(&r, "peer HELLO: incarnation 6, expected 5"));
	talloc_free(w0);
	talloc_free(w1);
}

/***********************************************************************
 * R-1: the osmo-bsc command line
 ***********************************************************************/

static const char *g_bsc;

/* Run osmo-bsc with args (NULL-terminated); \returns its exit code (-1: killed), its stderr in err. */
static int run_bsc(const char *const *args, char *err, size_t errlen)
{
	char *argv[16], outpath[300];
	unsigned int n = 0;
	unsigned long end;
	int status, fd;
	pid_t pid;

	argv[n++] = (char *)g_bsc;
	while (*args && n < ARRAY_SIZE(argv) - 1)
		argv[n++] = (char *)*args++;
	argv[n] = NULL;
	snprintf(outpath, sizeof(outpath), "%s/bsc.err", g_dir);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || dup2(fd, 2) != 2)
			_exit(127);
		fd = open("/dev/null", O_RDWR);
		if (fd < 0 || dup2(fd, 1) != 1 || dup2(fd, 0) != 0)
			_exit(127);
		execv(g_bsc, argv);
		_exit(127);
	}
	end = now_ms() + 30000;
	while (waitpid(pid, &status, WNOHANG) != pid) {
		if (now_ms() > end) {
			kill(pid, SIGKILL);
			OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
			break;
		}
		usleep(2000);
	}
	err[0] = '\0';
	fd = open(outpath, O_RDONLY);
	if (fd >= 0) {
		slurp_fd(fd, err, errlen);
		close(fd);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void r1_case(const char *label, const char *const *args, int expect_rc, const char *expect_err)
{
	char err[8192];
	int rc = run_bsc(args, err, sizeof(err));
	bool said = !expect_err || strstr(err, expect_err);

	printf("  %s: exit %d%s\n", label, rc, said ? "" : " (NOT the expected message)");
	CHECK(rc == expect_rc);
	CHECK(said);
	if (!said)
		printf("    expected '%s'\n", expect_err);
}

static void part_r1(void)
{
	char out[8192];

	printf("R-1: the osmo-bsc command line (descriptors 900 and up are not open)\n");
	{
		const char *a[] = { "--aworker-snapshot-fd", "901", NULL };
		r1_case("--aworker-snapshot-fd without --aworker-fd", a, 2, "--aworker-snapshot-fd requires --aworker-fd");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "0", "--aworker-snapshot-fd", "901",
				    "--aworker-config", "/nonexistent/w.cfg", NULL };
		r1_case("with --aworker-config as well", a, 2, "cannot be combined");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "0", "--aworker-snapshot-fd", "900", NULL };
		r1_case("the same descriptor as --aworker-fd", a, 2, "must differ from --aworker-fd");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "0", "--aworker-snapshot-fd", "2", NULL };
		r1_case("descriptor 2", a, 2, "needs a file descriptor above 2");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "0", "--aworker-snapshot-fd", "0", NULL };
		r1_case("descriptor 0", a, 2, "needs a file descriptor above 2");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "0", "--aworker-snapshot-fd", "9x", NULL };
		r1_case("not a number", a, 2, "needs a file descriptor above 2");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-snapshot-fd", "901", NULL };
		r1_case("without --aworker-index", a, 2, "requires --aworker-index with a worker index from 0 to 15");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "abc", "--aworker-snapshot-fd", "901", NULL };
		r1_case("--aworker-index 'abc'", a, 2, "requires --aworker-index with a worker index from 0 to 15");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "+1", "--aworker-snapshot-fd", "901", NULL };
		r1_case("--aworker-index '+1'", a, 2, "requires --aworker-index with a worker index from 0 to 15");
	}
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "16", "--aworker-snapshot-fd", "901", NULL };
		r1_case("--aworker-index 16", a, 2, "requires --aworker-index with a worker index from 0 to 15");
	}
	{
		/* Valid: past the command line, the worker refuses the descriptor 901, which is not open. */
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "15", "--aworker-snapshot-fd", "901", NULL };
		r1_case("valid options, descriptor not open", a, AWORKER_EXIT_CONFIG, "configuration snapshot refused");
	}
	/* Legacy, unchanged: a malformed index is taken as atoi() takes it; the IPC link on 900 fails. */
	{
		const char *a[] = { "--aworker-fd", "900", "--aworker-index", "abc", "--aworker-config", "/nonexistent/w.cfg",
				    NULL };
		r1_case("legacy: --aworker-index 'abc' (not checked)", a, AWORKER_EXIT_LINK,
			"cannot set up the IPC link on fd 900");
	}
	{
		const char *a[] = { "--aworker-fd", "900", NULL };
		r1_case("legacy: --aworker-fd without --aworker-config", a, 2, "--aworker-fd requires --aworker-config");
	}
	{
		const char *a[] = { "--aworker-config", "/nonexistent/w.cfg", NULL };
		r1_case("legacy: --aworker-config without --aworker-fd", a, 2, "--aworker-config requires --aworker-fd");
	}
	{
		const char *a[] = { "--help", NULL };
		char *argv0 = (char *)g_bsc;
		int rc;
		/* --help goes to stdout: run it with stdout captured. */
		char cmd[600];
		FILE *p;
		snprintf(cmd, sizeof(cmd), "'%s' --help 2>/dev/null", argv0);
		p = popen(cmd, "r");
		OSMO_ASSERT(p);
		slurp_fd(fileno(p), out, sizeof(out));
		rc = pclose(p);
		(void)a;
		printf("  --help: exit %d; lists --aworker-snapshot-fd: %s\n", WEXITSTATUS(rc),
		       strstr(out, "--aworker-snapshot-fd N\tConfiguration snapshot of this worker") ? "yes" : "NO");
		CHECK(WEXITSTATUS(rc) == 0);
		CHECK(strstr(out, "--aworker-snapshot-fd N\tConfiguration snapshot of this worker"));
	}
}

int main(int argc, char **argv)
{
	char cmd[300];

	g_bsc = argc > 1 ? argv[1] : "../../src/osmo-bsc/osmo-bsc";
	ctx = talloc_named_const(NULL, 0, "aworker_worker_test");
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	log_set_use_color(osmo_stderr_target, 0);
	vty_info.tall_ctx = ctx;
	vty_init(&vty_info);
	logging_vty_add_cmds();
	OSMO_ASSERT(osmo_ss7_init() == 0);
	osmo_ss7_vty_init_asp(ctx);
	osmo_sccp_vty_init();
	aworker_pool_vty_init(ctx);
	/* aworker_ipc_link_alloc() needs SIGPIPE ignored (the main process side here, as in osmo-bsc). */
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_worker.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	g_port = free_port();
	do {
		g_port2 = free_port();
	} while (g_port2 == g_port);

	part_w();
	part_r1();

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	talloc_free(ctx);
	return failures ? 1 : 0;
}

/* Tests of the real bsc-aworker executable (S2-3): options, configuration errors, and the pool it runs */
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

/* The binary under test is argv[1]. The Supervisor has no VTY (D-S6), so it is observed from outside: its log (stderr,
 * which its workers share), /proc, signals and inotify. Its workers' ASPs point at a TCP port on 127.0.0.1 that is
 * free when the test starts: they never connect, and nothing outside the test is contacted. This process is a child
 * subreaper, so that the workers of a Supervisor it kills become its children and can be collected.
 *
 * O: options and the worker role's internal options (B4)
 * C: configuration errors of the Supervisor role (D-S1, D-S8)
 * L: the pool: start, workers' image and descriptors, restart from the original snapshot, restart limit, SIGTERM
 *    (GOODBYE, exit 0), SIGKILL of the Supervisor (no worker survives it), the executable replaced on disk */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *g_bin;
static const char *g_extra_cfg = "";	/* appended to every configuration write_cfg() writes */
static char g_dir[256];
static unsigned int g_port;
static unsigned int failures;

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			fflush(stdout); \
			failures++; \
		} \
	} while (0)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static unsigned long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void sleep_ms(unsigned int ms)
{
	struct timespec t = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
	nanosleep(&t, NULL);
}

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

static void path_of(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/%s", g_dir, name);
}

static void write_path(const char *p, const char *text)
{
	FILE *f = fopen(p, "w");
	if (!f || fputs(text, f) < 0 || fclose(f))
		abort();
}

/* The contents of a file (static buffer, truncated at 256 KiB). */
static const char *slurp(const char *p)
{
	static char buf[262144];
	ssize_t n = 0, r;
	int fd = open(p, O_RDONLY);

	buf[0] = '\0';
	if (fd < 0)
		return buf;
	while (n < (ssize_t)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
		n += r;
	buf[n] = '\0';
	close(fd);
	return buf;
}

/***********************************************************************
 * Running the binary
 ***********************************************************************/

/* Start g_bin with args (NULL-terminated), stdout and stderr appended to log, in directory cwd (NULL: g_dir). */
static pid_t start(const char *const *args, const char *log, const char *cwd)
{
	char *argv[24];
	unsigned int n = 0;
	pid_t pid;
	int fd;

	argv[n++] = (char *)g_bin;
	while (*args && n < ARRAY_SIZE(argv) - 1)
		argv[n++] = (char *)*args++;
	argv[n] = NULL;
	fflush(stdout);
	pid = fork();
	if (pid < 0)
		abort();
	if (pid == 0) {
		if (chdir(cwd ? cwd : g_dir))
			_exit(127);
		fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
		if (fd < 0 || dup2(fd, 1) != 1 || dup2(fd, 2) != 2)
			_exit(127);
		close(fd);
		fd = open("/dev/null", O_RDONLY);
		if (fd < 0 || dup2(fd, 0) != 0)
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	return pid;
}

/* Wait for pid (bounded); \returns its exit status as by waitpid(), -1 if it did not end (it is then killed). */
static int wait_end(pid_t pid, unsigned int ms)
{
	unsigned long end = now_ms() + ms;
	int status;

	while (now_ms() < end) {
		if (waitpid(pid, &status, WNOHANG) == pid)
			return status;
		sleep_ms(10);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return -1;
}

/* Run g_bin to its end; \returns its exit code (-1: killed or timed out); its output in *out (static). */
static int run(const char *const *args, const char **out)
{
	char log[300];
	int status;

	path_of(log, sizeof(log), "run.log");
	unlink(log);
	status = wait_end(start(args, log, NULL), 20000);
	*out = slurp(log);
	return status != -1 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void expect_run(const char *label, const char *const *args, int rc_want, const char *text)
{
	const char *out;
	int rc = run(args, &out);
	bool said = !text || strstr(out, text);

	printf("  %s: exit %d%s\n", label, rc, said ? "" : " (NOT the expected message)");
	CHECK(rc == rc_want);
	CHECK(said);
	if (!said)
		printf("    output: %s\n", out);
}

/***********************************************************************
 * /proc helpers
 ***********************************************************************/

static bool alive(pid_t pid)
{
	char p[64], st[512], *e;
	FILE *f;
	bool r = false;

	snprintf(p, sizeof(p), "/proc/%d/stat", (int)pid);
	f = fopen(p, "r");
	if (!f)
		return false;
	if (fgets(st, sizeof(st), f) && (e = strrchr(st, ')')))
		r = e[2] != 'Z' && e[2] != 'X';
	fclose(f);
	return r;
}

static pid_t ppid_of(pid_t pid)
{
	char p[64], st[512], *e;
	int pp = -1;
	FILE *f;

	snprintf(p, sizeof(p), "/proc/%d/stat", (int)pid);
	f = fopen(p, "r");
	if (!f)
		return -1;
	if (fgets(st, sizeof(st), f) && (e = strrchr(st, ')')))
		sscanf(e + 2, "%*c %d", &pp);
	fclose(f);
	return pp;
}

/* The command line of pid, arguments separated by blanks (static buffer). */
static const char *cmdline(pid_t pid)
{
	static char buf[2048];
	char p[64];
	ssize_t n;
	int fd, i;

	snprintf(p, sizeof(p), "/proc/%d/cmdline", (int)pid);
	buf[0] = '\0';
	fd = open(p, O_RDONLY);
	if (fd < 0)
		return buf;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	n = n < 0 ? 0 : n;
	buf[n] = '\0';
	for (i = 0; i < n - 1; i++)
		if (!buf[i])
			buf[i] = ' ';
	return buf;
}

static bool same_image(pid_t a, pid_t b)
{
	char pa[64], pb[64];
	struct stat sa, sb;

	snprintf(pa, sizeof(pa), "/proc/%d/exe", (int)a);
	snprintf(pb, sizeof(pb), "/proc/%d/exe", (int)b);
	return !stat(pa, &sa) && !stat(pb, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
}

/* Counts over pid's descriptors: snapshots (memfd or fallback file), links to path, sockets that are not AF_UNIX. */
struct fdinfo {
	unsigned int memfd[16], fallback, path_links, inet_sockets, inet_listen;
};

static bool inode_in(const char *table, unsigned long inode, bool *listening)
{
	char line[512];
	FILE *f = fopen(table, "r");
	bool found = false;

	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		char st[8];
		unsigned long ino;
		if (sscanf(line, " %*s %*s %*s %7s %*s %*s %*s %*s %*s %lu", st, &ino) == 2 && ino == inode) {
			found = true;
			*listening = !strcmp(st, "0A");
			break;
		}
	}
	fclose(f);
	return found;
}

static struct fdinfo fdinfo(pid_t pid, const char *path)
{
	static const char *const tables[] = { "tcp", "tcp6", "udp", "udp6", "raw", "raw6" };
	struct fdinfo r = {};
	char dp[64], p[64 + sizeof(((struct dirent *)0)->d_name)], t[300], tp[64];
	struct dirent *de;
	DIR *d;

	snprintf(dp, sizeof(dp), "/proc/%d/fd", (int)pid);
	d = opendir(dp);
	if (!d)
		return r;
	while ((de = readdir(d))) {
		ssize_t l;
		unsigned int i;
		snprintf(p, sizeof(p), "%s/%s", dp, de->d_name);
		l = readlink(p, t, sizeof(t) - 1);
		if (l <= 0)
			continue;
		t[l] = '\0';
		if (!strncmp(t, "/memfd:bsc-aworker-", 19)) {
			unsigned int idx = atoi(t + 19);
			if (idx < 16)
				r.memfd[idx]++;
		}
		if (strstr(t, "/bsc-aworker-snap."))
			r.fallback++;
		if (path && !strcmp(t, path))
			r.path_links++;
		if (!strncmp(t, "socket:[", 8)) {
			unsigned long inode = strtoul(t + 8, NULL, 10);
			for (i = 0; i < ARRAY_SIZE(tables); i++) {
				bool listening = false;
				snprintf(tp, sizeof(tp), "/proc/%d/net/%s", (int)pid, tables[i]);
				if (inode_in(tp, inode, &listening)) {
					r.inet_sockets++;
					r.inet_listen += listening;
				}
			}
		}
	}
	closedir(d);
	return r;
}

/***********************************************************************
 * The configuration and the Supervisor's log
 ***********************************************************************/

/* An inline pool of two workers. pc: the BSC point codes are 0.23.<pc> and 0.23.<pc+1>; pool: extra 'a-worker-pool'
 * lines. */
static void write_cfg(const char *p, unsigned int pc, const char *pool)
{
	char text[4096];

	snprintf(text, sizeof(text),
		 "log stderr\n"
		 " logging filter all 1\n"
		 " logging color 0\n"
		 " logging print category 1\n"
		 " logging print file 0\n"
		 " logging level set-all notice\n"
		 " logging level aworker info\n"
		 " logging filter imsi 001010000000001\n"
		 "line vty\n"
		 " no login\n"
		 "cs7 instance 0\n"
		 " point-code 0.23.%u\n"
		 " asp asp-w0 %u 0 m3ua tcp\n"
		 "  remote-ip 127.0.0.1\n"
		 "  local-ip 127.0.0.1\n"
		 "  role asp\n"
		 "  transport-role client\n"
		 "cs7 instance 1\n"
		 " point-code 0.23.%u\n"
		 " asp asp-w1 %u 0 m3ua tcp\n"
		 "  remote-ip 127.0.0.1\n"
		 "  local-ip 127.0.0.1\n"
		 "  role asp\n"
		 "  transport-role client\n"
		 "a-worker-pool\n"
		 " workers 2\n"
		 " worker 0 cs7-instance 0\n"
		 " worker 1 cs7-instance 1\n"
		 " startup-timeout 20\n"
		 " restart-delay 1\n"
		 " restart-delay-max 1\n"
		 "%s"
		 "network\n"
		 " network country code 001\n"
		 " mobile network code 01\n"
		 "%s",
		 pc, g_port, pc + 1, g_port, pool ? pool : "", g_extra_cfg);
	write_path(p, text);
}

/* The pid of the process of worker idx that is READY ("owns cs7 instance"), the newest one after the first 'after'
 * octets of log; 0 if none. */
static pid_t ready_pid(const char *log, unsigned int idx, size_t after, unsigned int *pc)
{
	const char *s = slurp(log), *at;
	char needle[64];
	pid_t pid = 0;

	snprintf(needle, sizeof(needle), "aworker %u (pid ", idx);
	if (after > strlen(s))
		return 0;
	for (at = strstr(s + after, needle); at; at = strstr(at + 1, needle)) {
		int p;
		unsigned int c, v;
		if (sscanf(at + strlen(needle), "%d): owns cs7 instance %u: point-code %u", &p, &c, &v) == 3) {
			pid = p;
			if (pc)
				*pc = v;
		}
	}
	return pid;
}

static pid_t wait_ready(const char *log, unsigned int idx, size_t after, unsigned int *pc, unsigned int ms)
{
	unsigned long end = now_ms() + ms;
	pid_t pid;

	while (!(pid = ready_pid(log, idx, after, pc)) && now_ms() < end)
		sleep_ms(20);
	return pid;
}

static bool wait_log(const char *log, const char *text, unsigned int ms)
{
	unsigned long end = now_ms() + ms;

	while (now_ms() < end) {
		if (strstr(slurp(log), text))
			return true;
		sleep_ms(20);
	}
	return false;
}

static bool wait_gone(pid_t pid, unsigned int ms)
{
	unsigned long end = now_ms() + ms;
	int status;

	while (now_ms() < end) {
		waitpid(-1, &status, WNOHANG);	/* collect re-parented workers (subreaper) */
		if (!alive(pid))
			return true;
		sleep_ms(10);
	}
	return false;
}

/***********************************************************************
 * O: options
 ***********************************************************************/

static void part_o(void)
{
	const char *out;
	int rc;

	printf("O: options; the worker role's internal options (B4)\n");
	rc = run((const char *[]){ "--help", NULL }, &out);
	printf("  --help: exit %d; lists -c: %s; mentions a worker option: %s\n", rc,
	       strstr(out, "-c --config-file") ? "yes" : "NO", strstr(out, "aworker-") ? "YES" : "no");
	CHECK(rc == 0 && strstr(out, "-c --config-file") && !strstr(out, "aworker-"));
	expect_run("--version", (const char *[]){ "--version", NULL }, 0, "bsc-aworker version ");
	expect_run("an unknown option", (const char *[]){ "--no-such-option", NULL }, 2, "invalid option");
	expect_run("an argument that is not an option", (const char *[]){ "extra", NULL }, 2, "unexpected argument");
	expect_run("--aworker-fd alone", (const char *[]){ "--aworker-fd", "900", NULL }, 2,
		   "needs --aworker-fd, --aworker-index and --aworker-snapshot-fd");
	expect_run("--aworker-config (osmo-bsc's file form)", (const char *[]){ "--aworker-fd", "900",
		   "--aworker-index", "0", "--aworker-config", "/nonexistent", NULL }, 2,
		   "--aworker-config is not supported");
	expect_run("the same descriptor twice", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "0",
		   "--aworker-snapshot-fd", "900", NULL }, 2, "must differ from --aworker-fd");
	expect_run("descriptor 2", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "0",
		   "--aworker-snapshot-fd", "2", NULL }, 2, "needs a file descriptor above 2");
	expect_run("--aworker-index '+1'", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "+1",
		   "--aworker-snapshot-fd", "901", NULL }, 2, "worker index from 0 to 15");
	expect_run("--aworker-index 16", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "16",
		   "--aworker-snapshot-fd", "901", NULL }, 2, "worker index from 0 to 15");
	expect_run("-c with the worker role", (const char *[]){ "-c", "x.cfg", "--aworker-fd", "900", "--aworker-index",
		   "0", "--aworker-snapshot-fd", "901", NULL }, 2, "cannot be combined with the worker role");
	expect_run("--aworker-incarnation 0", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "0",
		   "--aworker-snapshot-fd", "901", "--aworker-incarnation", "0", NULL }, 2, "needs a number from 1");
	expect_run("--aworker-incarnation 'x'", (const char *[]){ "--aworker-fd", "900", "--aworker-index", "0",
		   "--aworker-snapshot-fd", "901", "--aworker-incarnation", "x", NULL }, 2, "needs a number from 1");
	expect_run("valid worker options, snapshot descriptor not open", (const char *[]){ "--aworker-fd", "900",
		   "--aworker-index", "1", "--aworker-snapshot-fd", "901", NULL }, 66, "configuration snapshot refused");
}

/***********************************************************************
 * C: configuration errors
 ***********************************************************************/

static void part_c(void)
{
	char cfg[300], empty[300], text[4096];

	printf("C: configuration errors of the Supervisor role (exit 1)\n");
	path_of(cfg, sizeof(cfg), "c.cfg");
	unlink(cfg);
	expect_run("no such file", (const char *[]){ "-c", cfg, NULL }, 1, "cannot read it");
	path_of(empty, sizeof(empty), "empty");
	mkdir(empty, 0700);
	{
		char log[300];
		int status;
		const char *out;
		path_of(log, sizeof(log), "run.log");
		unlink(log);
		status = wait_end(start((const char *[]){ NULL }, log, empty), 20000);
		out = slurp(log);
		printf("  no -c, no osmo-bsc.cfg in the working directory: exit %d, %s\n",
		       WIFEXITED(status) ? WEXITSTATUS(status) : -1,
		       strstr(out, "osmo-bsc.cfg: cannot read it") ? "names osmo-bsc.cfg" : "(NOT the expected message)");
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1 && strstr(out, "osmo-bsc.cfg: cannot read it"));
	}
	snprintf(text, sizeof(text), "a-worker-pool\n workers 1\n worker 0 config-file /etc/w0.cfg\n");
	write_path(cfg, text);
	expect_run("the file form", (const char *[]){ "-c", cfg, NULL }, 1, "accepts only the inline form");
	write_cfg(cfg, 3, " on-worker-failure shutdown\n");
	expect_run("'on-worker-failure shutdown' (D-S8)", (const char *[]){ "-c", cfg, NULL }, 1, "(D-S8)");
	write_path(cfg, "log stderr\n logging level set-all notice\nnetwork\n network country code 001\n");
	expect_run("no 'a-worker-pool'", (const char *[]){ "-c", cfg, NULL }, 1, "configures no A-worker pool");
}

/***********************************************************************
 * L: the pool
 ***********************************************************************/

struct sup {
	pid_t pid;
	char log[300];
	char cfg[300];
	pid_t w[2];
	unsigned int pc[2];
};

static bool sup_start(struct sup *s, const char *tag, const char *pool, const char *bin)
{
	const char *saved = g_bin;

	memset(s, 0, sizeof(*s));
	snprintf(s->log, sizeof(s->log), "%s/%s.log", g_dir, tag);
	snprintf(s->cfg, sizeof(s->cfg), "%s/%s.cfg", g_dir, tag);
	unlink(s->log);
	write_cfg(s->cfg, 3, pool);
	if (bin)
		g_bin = bin;
	s->pid = start((const char *[]){ "-c", s->cfg, NULL }, s->log, NULL);
	g_bin = saved;
	s->w[0] = wait_ready(s->log, 0, 0, &s->pc[0], 20000);
	s->w[1] = wait_ready(s->log, 1, 0, &s->pc[1], 20000);
	if (!s->w[0] || !s->w[1])
		printf("  (workers not READY; log: %s)\n", slurp(s->log));
	return s->w[0] && s->w[1];
}

/* SIGTERM: GOODBYE to every worker, which exits 0 by itself; then the Supervisor exits 0. */
static void sup_stop(struct sup *s, bool report)
{
	int status, i;
	pid_t w[2] = { ready_pid(s->log, 0, 0, NULL), ready_pid(s->log, 1, 0, NULL) };
	char text[64];
	bool goodbye = true;

	kill(s->pid, SIGTERM);
	status = wait_end(s->pid, 20000);
	for (i = 0; i < 2; i++) {
		snprintf(text, sizeof(text), "(pid %d): exited with status 0", (int)w[i]);
		goodbye &= w[i] && wait_log(s->log, text, 2000);
	}
	CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(goodbye);
	CHECK(!alive(w[0]) && !alive(w[1]));
	if (report)
		printf("  SIGTERM: both workers got GOODBYE and exited 0 by themselves; the Supervisor exited 0\n");
}

/* How often text names osmo-bsc, not counting source file names in a path below a directory 'osmo-bsc-...' (lines
 * logged before a worker applied its logging configuration show the file name, and the worktree's directory may be
 * called so). */
static unsigned int names_osmo_bsc(const char *text)
{
	unsigned int n = 0;
	const char *at;

	for (at = strstr(text, "osmo-bsc"); at; at = strstr(at + 1, "osmo-bsc")) {
		const char *slash = strchr(at, '/'), *space = strpbrk(at, " \n");
		if (at > text && at[-1] == '/' && slash && (!space || slash < space))
			continue;	/* a directory in a path */
		n++;
	}
	return n;
}

static void part_l(void)
{
	struct sup s;
	struct fdinfo fi;
	char moved[sizeof(((struct sup *)0)->cfg) + 8], text[128];
	unsigned int pc, i;
	int ino, wd_file, wd_dir;
	pid_t w1, w0;
	size_t mark;
	bool ok;

	printf("L: the pool run by the Supervisor\n");

	/* L1: start */
	CHECK(sup_start(&s, "l1", NULL, NULL));
	ok = true;
	for (i = 0; i < 2; i++) {
		ok &= ppid_of(s.w[i]) == s.pid;
		ok &= same_image(s.w[i], s.pid);
		ok &= strstr(cmdline(s.w[i]), "--aworker-snapshot-fd") && !strstr(cmdline(s.w[i]), "--aworker-config")
		      && !strstr(cmdline(s.w[i]), " -c ");
	}
	CHECK(ok);
	printf("  both workers READY: children of the Supervisor, its own image, started with --aworker-fd,"
	       " --aworker-index and --aworker-snapshot-fd\n");
	fi = fdinfo(s.pid, s.cfg);
	CHECK(fi.memfd[0] == 1 && fi.memfd[1] == 1 && fi.fallback == 0 && fi.path_links == 0);
	CHECK(fi.inet_sockets == 0 && fi.inet_listen == 0);
	printf("  the Supervisor holds the two sealed snapshots and not the file; it has no network socket at all (no"
	       " VTY, CTRL or other server)\n");
	for (i = 0; i < 2; i++) {
		fi = fdinfo(s.w[i], s.cfg);
		CHECK(!fi.memfd[0] && !fi.memfd[1] && !fi.fallback && !fi.path_links && !fi.inet_listen);
	}
	printf("  a running worker holds no snapshot and not the file\n");
	CHECK(strstr(slurp(s.log), "A-worker starting: pid") != NULL);

	/* B5: the Supervisor's lines and the workers' lines say who wrote them; nothing says osmo-bsc. */
	{
		const char *l = slurp(s.log);
		ok = strstr(l, "aworker-supervisor: A-worker pool started: 2 worker(s)")
		     && strstr(l, "aworker-supervisor: aworker 0 (pid ") && strstr(l, "aworker-supervisor: aworker 1 (pid ")
		     && strstr(l, "aworker 0 inc 1: A-worker starting: pid ") && strstr(l, "aworker 1 inc 1: A-worker starting: pid ")
		     && strstr(l, "ident 'bsc-aworker')") && names_osmo_bsc(l) == 0;
		CHECK(ok);
		if (!ok)
			printf("    log: %s\n", l);
	}
	printf("  log lines: the Supervisor's begin 'aworker-supervisor: ', a worker's 'aworker <N> inc <start>: '; the"
	       " IPC peer is 'bsc-aworker'; no line names osmo-bsc\n");

	/* L2: the file moved away and another one put in its place: a restarted worker reads the original snapshot,
	 * and nobody opens the file. */
	ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	CHECK(ino >= 0);
	wd_file = inotify_add_watch(ino, s.cfg, IN_OPEN | IN_ACCESS);
	wd_dir = inotify_add_watch(ino, g_dir, IN_OPEN | IN_ACCESS);
	CHECK(wd_file >= 0 && wd_dir >= 0);
	snprintf(moved, sizeof(moved), "%s.moved", s.cfg);
	CHECK(rename(s.cfg, moved) == 0);
	write_cfg(s.cfg, 13, NULL);
	sleep_ms(100);
	{	/* drop the test's own events (writing the new file) */
		char ev[4096];
		while (read(ino, ev, sizeof(ev)) > 0)
			;
	}
	w0 = s.w[0];
	mark = strlen(slurp(s.log));
	kill(s.w[1], SIGKILL);
	w1 = wait_ready(s.log, 1, mark, &pc, 20000);
	CHECK(w1 && w1 != s.w[1] && pc == s.pc[1]);
	CHECK(ready_pid(s.log, 0, 0, NULL) == w0 && alive(w0));
	CHECK(strstr(slurp(s.log) + mark, "aworker 1 inc 2: A-worker starting: pid ") != NULL);
	{
		char ev[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
		ssize_t n = read(ino, ev, sizeof(ev));
		bool opened = false;
		ssize_t off = 0;
		while (n > 0 && off < n) {
			struct inotify_event *e = (struct inotify_event *)(ev + off);
			if (e->wd == wd_file || (e->len && strstr(e->name, ".cfg")))
				opened = true;
			off += sizeof(*e) + e->len;
		}
		CHECK(!opened);
	}
	close(ino);
	printf("  the file moved away, another put in its place, worker 1 SIGKILLed: its new process READY with the"
	       " original point code; nothing opened the file; worker 0 untouched\n");
	s.w[1] = w1;

	/* L3: SIGTERM */
	sup_stop(&s, true);

	/* L4: the restart limit: 'max-restarts 2 window 300' -- the second loss within 300 s makes worker 1 FAILED. */
	CHECK(sup_start(&s, "l4", " max-restarts 2 window 300\n", NULL));
	w0 = s.w[0];
	mark = strlen(slurp(s.log));
	kill(s.w[1], SIGKILL);
	w1 = wait_ready(s.log, 1, mark, NULL, 20000);
	CHECK(w1 && w1 != s.w[1]);
	kill(w1, SIGKILL);
	snprintf(text, sizeof(text), "aworker 1 (pid %d): FAILED (restart-limit)", (int)w1);
	CHECK(wait_log(s.log, text, 20000));
	/* There is no reset command in the Supervisor yet (operator channel: S2-5): the message must not offer one. */
	CHECK(strstr(slurp(s.log), "Fix the cause, then restart bsc-aworker.") != NULL);
	CHECK(strstr(slurp(s.log), "a-worker-pool reset worker") == NULL);
	sleep_ms(1500);
	CHECK(ready_pid(s.log, 1, 0, NULL) == w1 && !alive(w1));
	CHECK(ready_pid(s.log, 0, 0, NULL) == w0 && alive(w0));
	printf("  'max-restarts 2 window 300': worker 1 lost twice is FAILED (restart-limit) and not restarted; worker"
	       " 0 untouched; the message says to restart bsc-aworker (no reset command)\n");
	kill(s.pid, SIGTERM);
	{
		int status = wait_end(s.pid, 20000);
		CHECK(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
		CHECK(!alive(w0));
	}
	printf("  SIGTERM with a FAILED worker: exit 0, worker 0 gone\n");

	/* L5: SIGKILL of the Supervisor: no worker survives it (PR_SET_PDEATHSIG). */
	CHECK(sup_start(&s, "l5", NULL, NULL));
	kill(s.pid, SIGKILL);
	CHECK(wait_end(s.pid, 5000) != -1);
	CHECK(wait_gone(s.w[0], 5000) && wait_gone(s.w[1], 5000));
	printf("  the Supervisor SIGKILLed: both workers end with it\n");

	/* L6: the executable replaced on disk while the Supervisor runs: a restarted worker is still its image. */
	{
		char copy[300], cmd[700];
		struct stat before;

		path_of(copy, sizeof(copy), "bsc-aworker-copy");
		snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", g_bin, copy);
		CHECK(system(cmd) == 0);
		CHECK(stat(copy, &before) == 0);
		CHECK(sup_start(&s, "l6", NULL, copy));
		CHECK(unlink(copy) == 0);
		write_path(copy, "#!/bin/sh\nexit 3\n");
		chmod(copy, 0700);
		mark = strlen(slurp(s.log));
		kill(s.w[0], SIGKILL);
		w0 = wait_ready(s.log, 0, mark, NULL, 20000);
		CHECK(w0 && w0 != s.w[0] && same_image(w0, s.pid));
		{
			char pe[64];
			struct stat now;
			snprintf(pe, sizeof(pe), "/proc/%d/exe", (int)w0);
			CHECK(stat(pe, &now) == 0 && now.st_ino == before.st_ino);
		}
		printf("  the executable replaced on disk: worker 0's new process runs the image the Supervisor was started"
		       " from\n");
		s.w[0] = w0;
		sup_stop(&s, false);
	}

	/* L7: D-S10 (B2): the Supervisor keeps its 'log file' target; workers do not open it and log to stderr. */
	{
		char shared[300], extra[512];
		const char *f;

		path_of(shared, sizeof(shared), "shared.log");
		snprintf(extra, sizeof(extra), "log file %s\n logging filter all 1\n logging color 0\n"
			 " logging print category 1\n logging print file 0\n logging level set-all notice\n logging level aworker info\n", shared);
		g_extra_cfg = extra;
		CHECK(sup_start(&s, "l7", NULL, NULL));
		g_extra_cfg = "";
		CHECK(fdinfo(s.pid, shared).path_links == 1);
		CHECK(fdinfo(s.w[0], shared).path_links == 0 && fdinfo(s.w[1], shared).path_links == 0);
		f = slurp(shared);
		CHECK(strstr(f, "aworker-supervisor: A-worker pool started") && strstr(f, "aworker-supervisor: aworker 1 (pid ")
		      && !strstr(f, "A-worker starting"));
		CHECK(strstr(slurp(s.log), "aworker 0 inc 1: A-worker starting") != NULL);
		printf("  'log file' configured: the Supervisor writes its lines there; its workers hold no descriptor of it"
		       " and their lines are on stderr only\n");
		sup_stop(&s, false);
	}
}

int main(int argc, char **argv)
{
	char cmd[300];

	if (argc != 2) {
		fprintf(stderr, "usage: %s PATH-OF-bsc-aworker\n", argv[0]);
		return 2;
	}
	g_bin = argv[1];
	if (access(g_bin, X_OK)) {
		fprintf(stderr, "%s: not executable\n", g_bin);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	snprintf(g_dir, sizeof(g_dir), "%s/aworker_supervisor_proc.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(g_dir))
		return 1;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0))
		return 1;
	g_port = free_port();

	part_o();
	part_c();
	part_l();

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

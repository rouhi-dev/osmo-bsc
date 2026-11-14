/* A-worker pool with REAL worker processes: fork(), exec(), termination, collection, descriptor hygiene,
 * parent death, executable consistency. */
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

/* What this test is, and what it is not
 * =====================================
 *
 * The pool is started with aworker_pool_start(), i.e. with the real socketpair(), fork(), PR_SET_PDEATHSIG and
 * exec() of /proc/self/exe. /proc/self/exe is this test binary: started with --aworker-fd it acts as a minimal
 * worker -- it speaks the real IPC protocol through the real aworker_ipc_link (HELLO, CS7_INFO, GOODBYE), reports
 * the descriptors it inherited, and behaves as its config file says (ignore SIGTERM, survive IPC EOF, ...).
 *
 * It does NOT run libosmo-sigtran: no SCTP, M3UA or SCCP. It proves process and resource isolation of the pool --
 * not SIGTRAN-level isolation, which needs an end-to-end test with osmo-stp and an MSC.
 *
 * Two link-time seams (see Makefile.am, -Wl,--wrap): waitpid() can be told that one process cannot be collected
 * (to test a process stuck in the kernel), and prctl(PR_SET_PDEATHSIG) can be delayed (to open the window between
 * fork() and prctl() in which the main process may die). */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
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
#include <osmocom/core/timer.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/debug.h>

void *tall_bsc_ctx;

static void *ctx;
static unsigned int failures;
static char g_dir[256];	/* a mkdtemp() directory; short, so that the paths built from it always fit */

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			fflush(stdout); \
			failures++; \
		} \
	} while (0)

static const struct log_info_cat test_cat[] = {
	[DAWORKER] = { .name = "DAWORKER", .description = "A-worker pool", .enabled = 1,
		       .loglevel = LOGL_INFO },
};
static const struct log_info test_log_info = { .cat = test_cat, .num_cat = ARRAY_SIZE(test_cat) };

/***********************************************************************
 * Link-time seams
 ***********************************************************************/

static pid_t g_stuck_pid;		/*!< waitpid() pretends this process cannot be collected */
static unsigned int g_prctl_delay_ms;	/*!< sleep this long before PR_SET_PDEATHSIG (in the forked child) */

pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
	if (g_stuck_pid > 0 && pid == g_stuck_pid && (options & WNOHANG))
		return 0;
	return __real_waitpid(pid, status, options);
}

int __real_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5);
int __wrap_prctl(int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5)
{
	if (option == PR_SET_PDEATHSIG && g_prctl_delay_ms)
		usleep(g_prctl_delay_ms * 1000);
	return __real_prctl(option, a2, a3, a4, a5);
}

static unsigned int g_forks;		/*!< fork() calls of this process (the pool starts a worker with fork()) */

pid_t __real_fork(void);
pid_t __wrap_fork(void)
{
	g_forks++;
	return __real_fork();
}

/***********************************************************************
 * Helpers
 ***********************************************************************/

static unsigned long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool pid_gone(pid_t pid)
{
	return kill(pid, 0) < 0 && errno == ESRCH;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

/* Descriptors of this process, sorted, without the one used to read the directory. \returns the count. */
static int list_fds(int *out, int max)
{
	struct dirent *de;
	DIR *d = opendir("/proc/self/fd");
	int n = 0;

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char *end;
		long fd = strtol(de->d_name, &end, 10);
		if (*end || end == de->d_name || fd == dirfd(d))
			continue;
		if (n < max)
			out[n++] = (int)fd;
	}
	closedir(d);
	qsort(out, n, sizeof(int), cmp_int);
	return n;
}

static void write_file(const char *path, const char *text)
{
	char tmp[PATH_MAX + 8];
	FILE *f;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "w");
	OSMO_ASSERT(f);
	fputs(text, f);
	fclose(f);
	OSMO_ASSERT(rename(tmp, path) == 0);
}

/***********************************************************************
 * The fake worker (this binary, exec'd by the pool with --aworker-fd)
 ***********************************************************************/

static struct {
	unsigned int idx;
	int cs7;
	bool ignore_term;
	bool stay_on_eof;
	char report[PATH_MAX];
	char heartbeat[PATH_MAX];
	char exitlog[PATH_MAX];		/*!< if set: how this worker ended ("goodbye" or "eof") */
	char termlog[PATH_MAX + 8];		/*!< with exitlog: created if SIGTERM arrives */
	unsigned int linger_ms;		/*!< on GOODBYE, exit only after this long (a worker shutting down) */
	struct osmo_timer_list exit_timer;
	bool got_goodbye;
	struct aworker_ipc_link *link;
	volatile sig_atomic_t sever;
	unsigned long beats;
	int hb_fd;
	struct osmo_timer_list timer;
} fw;

static void fw_ready_cb(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	struct aworker_ipc_cs7_info info = {
		.count = 1,
		.e[0] = { .cs7_id = fw.cs7, .primary_pc = 185, .asp_proto = OSMO_SS7_ASP_PROT_M3UA, .num_as = 1 },
	};
	struct msgb *rec;

	OSMO_ASSERT(aworker_ipc_enc_cs7_info(ctx, &rec, &info) == 0);
	aworker_ipc_link_send(link, rec, NULL);
}

static void fw_rx_cb(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	if (m->type == AWORKER_IPC_MT_GOODBYE) {
		fw.got_goodbye = true;
		if (fw.exitlog[0])
			write_file(fw.exitlog, "goodbye\n");
		if (!fw.linger_ms)
			exit(AWORKER_EXIT_OK);
		osmo_timer_schedule(&fw.exit_timer, fw.linger_ms / 1000, (fw.linger_ms % 1000) * 1000);
	}
}

static void fw_closed_cb(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	fw.link = NULL;
	if (fw.got_goodbye)
		return;	/* exiting anyway (like aworker.c, which is done with the link once it has GOODBYE) */
	if (fw.exitlog[0])
		write_file(fw.exitlog, "eof\n");
	if (!fw.stay_on_eof)
		exit(AWORKER_EXIT_LINK);
}

static const struct aworker_ipc_link_ops fw_ops = {
	.ready_cb = fw_ready_cb,
	.rx_cb = fw_rx_cb,
	.closed_cb = fw_closed_cb,
};

static void fw_exit_cb(void *data)
{
	exit(AWORKER_EXIT_OK);
}

static void fw_sigterm(int sig)
{
	int fd = open(fw.termlog, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
	if (fd >= 0)
		close(fd);
	_exit(128 + SIGTERM);
}

static void fw_sigusr1(int sig)
{
	fw.sever = 1;
}

/* Every 10 ms: count a heartbeat (proof that this worker's event loop runs) and act on SIGUSR1: drop the IPC link
 * but keep running -- a worker that is broken but alive, which the main process has to take down. */
static void fw_timer_cb(void *data)
{
	char buf[32];
	int len;

	fw.beats++;
	len = snprintf(buf, sizeof(buf), "%lu\n", fw.beats);
	if (pwrite(fw.hb_fd, buf, len, 0) < 0) {}
	if (fw.sever && fw.link) {
		aworker_ipc_link_free(fw.link);	/* closes the socket; no closed_cb */
		fw.link = NULL;
	}
	osmo_timer_schedule(&fw.timer, 0, 10000);
}

static int fake_worker_main(int argc, char **argv)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_WORKER,
		.ident = "aworker_proc_test",
		.log_cat = DAWORKER,
	};
	const char *cfg_file = NULL;
	int fd = -1, fds[64], n, i, len;
	char line[256], exe[PATH_MAX], text[2048];
	FILE *f;

	for (i = 1; i + 1 < argc; i++) {
		if (!strcmp(argv[i], "--aworker-fd"))
			fd = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--aworker-index"))
			fw.idx = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--aworker-config"))
			cfg_file = argv[++i];
	}
	if (fd < 0 || !cfg_file)
		return 2;

	/* First of all, before this process opens anything: which descriptors did it inherit? */
	n = list_fds(fds, ARRAY_SIZE(fds));
	len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	exe[len > 0 ? len : 0] = '\0';

	f = fopen(cfg_file, "r");
	if (!f)
		return AWORKER_EXIT_CONFIG;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = '\0';
		if (!strncmp(line, "cs7=", 4))
			fw.cs7 = atoi(line + 4);
		else if (!strcmp(line, "ignore_term=1"))
			fw.ignore_term = true;
		else if (!strcmp(line, "stay_on_eof=1"))
			fw.stay_on_eof = true;
		else if (!strncmp(line, "report=", 7))
			osmo_strlcpy(fw.report, line + 7, sizeof(fw.report));
		else if (!strncmp(line, "heartbeat=", 10))
			osmo_strlcpy(fw.heartbeat, line + 10, sizeof(fw.heartbeat));
		else if (!strncmp(line, "exitlog=", 8))
			osmo_strlcpy(fw.exitlog, line + 8, sizeof(fw.exitlog));
		else if (!strncmp(line, "linger=", 7))
			fw.linger_ms = atoi(line + 7);
	}
	fclose(f);

	if (fw.report[0]) {
		len = snprintf(text, sizeof(text), "pid=%d ppid=%d ipc=%d exe=%s fds=", (int)getpid(), (int)getppid(),
			       fd, exe);
		for (i = 0; i < n && len < (int)sizeof(text) - 16; i++)
			len += snprintf(text + len, sizeof(text) - len, "%s%d", i ? "," : "", fds[i]);
		snprintf(text + len, sizeof(text) - len, "\n");
		write_file(fw.report, text);
	}

	if (fw.ignore_term) {
		signal(SIGTERM, SIG_IGN);
	} else if (fw.exitlog[0]) {
		snprintf(fw.termlog, sizeof(fw.termlog), "%s.term", fw.exitlog);
		signal(SIGTERM, fw_sigterm);
	}
	signal(SIGUSR1, fw_sigusr1);
	signal(SIGPIPE, SIG_IGN);

	ctx = talloc_named_const(NULL, 0, "fake_worker");
	osmo_init_logging2(ctx, &test_log_info);
	cfg.worker_idx = fw.idx;
	cfg.pid = getpid();
	if (aworker_ipc_link_alloc(&fw.link, ctx, fd, &cfg, &fw_ops, NULL) < 0)
		return AWORKER_EXIT_LINK;

	fw.hb_fd = fw.heartbeat[0] ? open(fw.heartbeat, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600) : -1;
	osmo_timer_setup(&fw.timer, fw_timer_cb, NULL);
	osmo_timer_setup(&fw.exit_timer, fw_exit_cb, NULL);
	osmo_timer_schedule(&fw.timer, 0, 10000);
	for (;;)
		osmo_select_main_ctx(0);
	return 0;
}

/***********************************************************************
 * The main process (the BSC side) helpers
 ***********************************************************************/

/* A 10 ms tick: wakes the event loop and measures the longest gap between two ticks -- if anything in the main
 * process blocked the event loop, that gap shows it. */
static struct osmo_timer_list tick_timer;
static unsigned long tick_last, tick_max_gap;

static void tick_cb(void *data)
{
	unsigned long t = now_ms();
	if (tick_last && t - tick_last > tick_max_gap)
		tick_max_gap = t - tick_last;
	tick_last = t;
	osmo_timer_schedule(&tick_timer, 0, 10000);
}

static void tick_reset(void)
{
	tick_last = 0;
	tick_max_gap = 0;
}

static void bsc_init(void)
{
	ctx = talloc_named_const(NULL, 0, "aworker_proc_test");
	tall_bsc_ctx = ctx;
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	log_set_use_color(osmo_stderr_target, 0);
	signal(SIGPIPE, SIG_IGN);
	osmo_timer_setup(&tick_timer, tick_cb, NULL);
	osmo_timer_schedule(&tick_timer, 0, 10000);
}

/* Write worker config i (the fake worker's "config file") and return its path. */
static const char *worker_cfg(unsigned int i, int cs7, const char *extra)
{
	static char path[AWORKER_POOL_MAX_WORKERS][PATH_MAX];
	char text[2048];

	snprintf(path[i], sizeof(path[i]), "%s/worker%u.cfg", g_dir, i);
	snprintf(text, sizeof(text), "cs7=%d\nreport=%s/report%u\nheartbeat=%s/heartbeat%u\n%s", cs7, g_dir, i,
		 g_dir, i, extra ? extra : "");
	write_file(path[i], text);
	return path[i];
}

static void pool_configure(unsigned int num, const char *const *files)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int i;

	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++)
		TALLOC_FREE(cfg->cfg_file[i]);
	cfg->num_workers = num;
	for (i = 0; i < num; i++)
		cfg->cfg_file[i] = talloc_strdup(tall_bsc_ctx, files[i]);
	cfg->restart_delay = 1;
	cfg->startup_timeout = 10;
	cfg->on_failure = AWORKER_FAILURE_RESTART;
}

static struct aworker_pool_worker_info winfo(unsigned int i)
{
	struct aworker_pool_worker_info info = {};
	OSMO_ASSERT(aworker_pool_worker_info(i, &info) == 0);
	return info;
}

/* Run the event loop until cond() is true or ms have passed. \returns cond()'s last value. */
static bool run_until(bool (*cond)(void *), void *data, unsigned long ms)
{
	unsigned long end = now_ms() + ms;

	while (!cond(data)) {
		if (now_ms() > end)
			return false;
		osmo_select_main_ctx(0);
	}
	return true;
}

static bool all_ready(void *data)
{
	return aworker_pool_all_ready();
}

struct report {
	int pid, ppid, ipc;
	char exe[PATH_MAX];
	char fds[1024];
};

static bool read_report(unsigned int i, struct report *r)
{
	char path[PATH_MAX];
	FILE *f;
	int n;

	snprintf(path, sizeof(path), "%s/report%u", g_dir, i);
	f = fopen(path, "r");
	if (!f)
		return false;
	n = fscanf(f, "pid=%d ppid=%d ipc=%d exe=%4095s fds=%1023s", &r->pid, &r->ppid, &r->ipc, r->exe, r->fds);
	fclose(f);
	return n == 5;
}

/* A worker must hold exactly stdio and its IPC socket -- nothing else of the main process. */
static bool report_fds_ok(const struct report *r)
{
	char expect[64];
	snprintf(expect, sizeof(expect), "0,1,2,%d", r->ipc);
	if (strcmp(r->fds, expect))
		printf("  (worker pid %d inherited descriptors %s, expected %s)\n", r->pid, r->fds, expect);
	return !strcmp(r->fds, expect);
}

static unsigned long read_heartbeat(unsigned int i)
{
	char path[PATH_MAX];
	unsigned long v = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/heartbeat%u", g_dir, i);
	f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

/* Many descriptors in the main process that a worker must not inherit: 300 of them (more than the 256 the old
 * child_close_fds() could hold) and one above 1023 (beyond the old fallback sweep). None is close-on-exec. */
static int g_extra[301], g_num_extra;

static void open_extra_fds(void)
{
	struct rlimit rl;
	int i;

	OSMO_ASSERT(getrlimit(RLIMIT_NOFILE, &rl) == 0);
	if (rl.rlim_cur < 2100 && rl.rlim_max >= 2100) {
		rl.rlim_cur = 2100;
		OSMO_ASSERT(setrlimit(RLIMIT_NOFILE, &rl) == 0);
	}
	for (i = 0; i < 300; i++) {
		g_extra[g_num_extra] = open("/dev/null", O_RDONLY);
		OSMO_ASSERT(g_extra[g_num_extra] >= 0);
		g_num_extra++;
	}
	g_extra[g_num_extra] = fcntl(g_extra[0], F_DUPFD, 2000);
	OSMO_ASSERT(g_extra[g_num_extra] >= 2000);
	g_num_extra++;
}

static void close_extra_fds(void)
{
	while (g_num_extra)
		close(g_extra[--g_num_extra]);
}

/* Collect a child of this process (or an orphan re-parented to it), waiting at most ms. \returns its pid or 0. */
static pid_t collect(pid_t pid, int *status, unsigned long ms)
{
	unsigned long end = now_ms() + ms;
	pid_t rc;

	do {
		rc = __real_waitpid(pid, status, WNOHANG);
		if (rc == pid)
			return pid;
		usleep(10000);
	} while (now_ms() < end);
	return 0;
}

/***********************************************************************
 * P1: two real workers -- one is lost, the other is not affected
 ***********************************************************************/

struct p1_watch {
	pid_t old0, pid1;
	bool saw_terminating, saw_sigkill, new_gen_checked, old_gone_before_new, w1_disturbed;
};

static bool p1_new_generation_ready(void *data)
{
	struct p1_watch *p = data;
	struct aworker_pool_worker_info w0 = winfo(0), w1 = winfo(1);

	if (w0.state == AWORKER_ST_TERMINATING)
		p->saw_terminating = true;
	if (w0.sigkill_sent)
		p->saw_sigkill = true;
	if (w1.state != AWORKER_ST_READY || w1.pid != p->pid1)
		p->w1_disturbed = true;
	/* The first moment a new process of worker 0 exists, the old one must already be collected (ESRCH: not
	 * even a zombie is left). */
	if (w0.pid > 0 && w0.pid != p->old0 && !p->new_gen_checked) {
		p->new_gen_checked = true;
		p->old_gone_before_new = pid_gone(p->old0);
	}
	return w0.state == AWORKER_ST_READY && w0.pid > 0 && w0.pid != p->old0;
}

struct p1_stuck {
	pid_t stuck, pid1;
	unsigned int starts;
	bool entered, left_terminating, respawned, w1_disturbed;
};

static bool p1_stuck_reported(void *data)
{
	struct p1_stuck *p = data;
	struct aworker_pool_worker_info w0 = winfo(0), w1 = winfo(1);

	/* Until the main process has seen the link go down, worker 0 is still READY; judge only from then on. */
	if (w0.state == AWORKER_ST_TERMINATING)
		p->entered = true;
	else if (p->entered)
		p->left_terminating = true;
	if (w0.starts != p->starts || (w0.pid && w0.pid != p->stuck))
		p->respawned = true;
	if (w1.state != AWORKER_ST_READY || w1.pid != p->pid1)
		p->w1_disturbed = true;
	return w0.reap_stuck;
}

static bool p1_w0_ready_new(void *data)
{
	struct p1_stuck *p = data;
	struct aworker_pool_worker_info w0 = winfo(0);
	return w0.state == AWORKER_ST_READY && w0.pid > 0 && w0.pid != p->stuck;
}

static void test_p1_isolation(char **argv)
{
	const char *files[2];
	struct report r0, r1, r0b;
	struct p1_watch watch = {};
	struct p1_stuck stuck = {};
	unsigned long hb_before;
	pid_t last0, last1;
	int status;

	printf("P1: two real worker processes; one is lost, the other is not affected\n");
	open_extra_fds();
	/* Worker 0 ignores SIGTERM: taking it down needs SIGKILL, i.e. the full grace period. */
	files[0] = worker_cfg(0, 0, "ignore_term=1\n");
	files[1] = worker_cfg(1, 1, NULL);
	pool_configure(2, files);
	aworker_pool_set_exec(argv[0], 1, argv);
	CHECK(aworker_pool_start(ctx) == 0);
	CHECK(run_until(all_ready, NULL, 10000));
	printf("  both workers started (fork + exec) and reported their cs7 instance\n");

	CHECK(read_report(0, &r0) && read_report(1, &r1));
	CHECK(r0.pid == winfo(0).pid && r1.pid == winfo(1).pid);
	CHECK(r0.ppid == getpid() && r1.ppid == getpid());
	CHECK(report_fds_ok(&r0) && report_fds_ok(&r1));
	printf("  with >300 descriptors (one above 1023) open in the main process, each worker holds only stdio and"
	       " its IPC socket\n");

	watch.old0 = winfo(0).pid;
	watch.pid1 = winfo(1).pid;
	run_until(all_ready, NULL, 100);	/* let the heartbeats run a little */
	hb_before = read_heartbeat(1);
	tick_reset();

	/* Worker 0 drops its IPC link but keeps running and ignores SIGTERM. */
	kill(watch.old0, SIGUSR1);
	CHECK(run_until(p1_new_generation_ready, &watch, 10000));
	CHECK(watch.saw_terminating);
	CHECK(watch.saw_sigkill);
	printf("  worker 0 was taken down: SIGTERM ignored, SIGKILL after the grace period\n");
	CHECK(watch.new_gen_checked && watch.old_gone_before_new);
	printf("  its replacement started only after the old process was collected\n");
	CHECK(!watch.w1_disturbed);
	CHECK(read_heartbeat(1) > hb_before);
	printf("  worker 1 stayed READY with the same process, and its event loop kept running\n");
	CHECK(tick_max_gap < 400);
	if (tick_max_gap >= 400)
		printf("  (longest event loop gap %lu ms)\n", tick_max_gap);
	printf("  the main event loop was never blocked while worker 0 was taken down\n");

	CHECK(run_until(all_ready, NULL, 5000));
	CHECK(read_report(0, &r0b) && r0b.pid == winfo(0).pid);
	CHECK(report_fds_ok(&r0b));
	printf("  the restarted worker 0 also holds only stdio and its IPC socket\n");

	/* A process that cannot be collected (e.g. stuck in the kernel): waitpid() reports it as running forever. */
	stuck.stuck = winfo(0).pid;
	stuck.starts = winfo(0).starts;
	stuck.pid1 = winfo(1).pid;
	g_stuck_pid = stuck.stuck;
	tick_reset();
	kill(stuck.stuck, SIGKILL);
	CHECK(run_until(p1_stuck_reported, &stuck, AWORKER_TERM_GRACE_MS + AWORKER_REAP_STUCK_MS + 5000));
	CHECK(stuck.entered && !stuck.left_terminating && !stuck.respawned);
	CHECK(winfo(0).sigkill_sent && winfo(0).reap_stuck && winfo(0).pid == stuck.stuck);
	CHECK(!stuck.w1_disturbed);
	CHECK(tick_max_gap < 400);
	printf("  a process that cannot be collected keeps its worker TERMINATING and is reported as stuck; no second"
	       " generation is started, worker 1 is unaffected\n");
	g_stuck_pid = 0;
	CHECK(run_until(p1_w0_ready_new, &stuck, AWORKER_REAP_POLL_MAX_MS + 5000));
	CHECK(pid_gone(stuck.stuck));
	printf("  once it can be collected, the worker is restarted\n");

	last0 = winfo(0).pid;
	last1 = winfo(1).pid;
	aworker_pool_stop();
	CHECK(pid_gone(last0) && pid_gone(last1));
	CHECK(__real_waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD);
	printf("  stopping the pool collects every worker (the one ignoring SIGTERM via SIGKILL): no process, no"
	       " zombie left\n");
	close_extra_fds();
}

/***********************************************************************
 * P2: the main process is killed -- its workers do not survive it
 ***********************************************************************/

static void test_p2_main_killed(char **argv)
{
	int pp[2], status;
	pid_t bsc, pids[2] = {};
	bool all_collected = true, by_sigterm = true;
	unsigned int i;

	printf("P2: the main process is SIGKILLed; its workers do not survive it\n");
	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		/* A main process with two workers that would survive the loss of the IPC link: only the parent-death
		 * signal can end them. */
		const char *files[2] = { worker_cfg(0, 0, "stay_on_eof=1\n"), worker_cfg(1, 1, "stay_on_eof=1\n") };
		pid_t p[2];
		close(pp[0]);
		pool_configure(2, files);
		aworker_pool_set_exec(argv[0], 1, argv);
		if (aworker_pool_start(ctx) < 0 || !run_until(all_ready, NULL, 10000))
			_exit(1);
		p[0] = winfo(0).pid;
		p[1] = winfo(1).pid;
		if (write(pp[1], p, sizeof(p)) != sizeof(p))
			_exit(1);
		for (;;)
			pause();
	}
	close(pp[1]);
	CHECK(read(pp[0], pids, sizeof(pids)) == sizeof(pids));
	close(pp[0]);
	kill(bsc, SIGKILL);
	CHECK(collect(bsc, &status, 2000) == bsc);

	for (i = 0; i < 2; i++) {
		/* We are a child subreaper: orphans are re-parented to us and can be collected here. */
		if (!pids[i] || collect(pids[i], &status, 3000) != pids[i]) {
			all_collected = false;
			if (pids[i])
				kill(pids[i], SIGKILL), collect(pids[i], &status, 1000);
			continue;
		}
		if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM)
			by_sigterm = false;
	}
	CHECK(all_collected);
	CHECK(by_sigterm);
	printf("  both workers were terminated by the parent-death signal (SIGTERM) and collected\n");
}

/***********************************************************************
 * P3: the main process dies between fork() and PR_SET_PDEATHSIG
 ***********************************************************************/

static void test_p3_pdeathsig_race(char **argv)
{
	char report[PATH_MAX];
	int pp[2], status;
	pid_t bsc, worker = 0;
	struct stat st;

	printf("P3: the main process dies between fork() and PR_SET_PDEATHSIG\n");
	snprintf(report, sizeof(report), "%s/report0", g_dir);
	unlink(report);
	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		const char *files[1] = { worker_cfg(0, 0, "stay_on_eof=1\n") };
		pid_t p;
		close(pp[0]);
		pool_configure(1, files);
		aworker_pool_set_exec(argv[0], 1, argv);
		/* The forked child sleeps before PR_SET_PDEATHSIG; this process exits right after fork(). */
		g_prctl_delay_ms = 300;
		if (aworker_pool_start(ctx) < 0)
			_exit(1);
		p = winfo(0).pid;
		if (write(pp[1], &p, sizeof(p)) != sizeof(p)) {}
		_exit(0);
	}
	close(pp[1]);
	CHECK(read(pp[0], &worker, sizeof(worker)) == sizeof(worker));
	close(pp[0]);
	CHECK(collect(bsc, &status, 2000) == bsc);

	if (!worker || collect(worker, &status, 3000) != worker) {
		CHECK(!"the orphaned worker did not exit");
		if (worker)
			kill(worker, SIGKILL), collect(worker, &status, 1000);
	} else {
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == AWORKER_EXIT_LINK);
	}
	CHECK(stat(report, &st) < 0);
	printf("  the child noticed it was orphaned and exited before exec: no worker without its main process\n");
}

/***********************************************************************
 * P4: the executable is replaced while the main process runs
 ***********************************************************************/

static bool w0_ready_not(void *data)
{
	pid_t old = *(pid_t *)data;
	struct aworker_pool_worker_info w0 = winfo(0);
	return w0.state == AWORKER_ST_READY && w0.pid > 0 && w0.pid != old;
}

/* Runs as a separate main process, started from a COPY of this binary. */
static int exe_test_main(char **argv)
{
	const char *files[1];
	char exe[PATH_MAX], self[PATH_MAX], link[64], target[PATH_MAX];
	pid_t first;
	ssize_t len;
	int fails = 0;

	bsc_init();
	len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len <= 0)
		return 10;
	self[len] = '\0';
	files[0] = worker_cfg(0, 0, NULL);
	pool_configure(1, files);
	aworker_pool_set_exec(argv[0], 1, argv);
	if (aworker_pool_start(ctx) < 0 || !run_until(all_ready, NULL, 10000))
		return 11;
	first = winfo(0).pid;

	/* Replace the executable: delete it and put a different program at the same path. */
	snprintf(exe, sizeof(exe), "%s", self);
	if (unlink(exe) < 0)
		return 12;
	write_file(exe, "#!/bin/sh\nexit 42\n");
	chmod(exe, 0755);

	kill(first, SIGKILL);
	if (!run_until(w0_ready_not, &first, 8000))
		fails++;
	/* The new worker runs the original (now deleted) image, not the file now at that path. */
	snprintf(link, sizeof(link), "/proc/%d/exe", (int)winfo(0).pid);
	len = readlink(link, target, sizeof(target) - 1);
	target[len > 0 ? len : 0] = '\0';
	if (!strstr(target, "(deleted)"))
		fails++;
	aworker_pool_stop();
	return fails ? 13 : 0;
}

static void test_p4_exe_replaced(void)
{
	char copy[PATH_MAX], buf[65536];
	int in, out, status;
	ssize_t n;
	pid_t pid;

	printf("P4: the executable file is replaced while the main process runs\n");
	snprintf(copy, sizeof(copy), "%s/bsc-copy", g_dir);
	in = open("/proc/self/exe", O_RDONLY);
	out = open(copy, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	OSMO_ASSERT(in >= 0 && out >= 0);
	while ((n = read(in, buf, sizeof(buf))) > 0)
		OSMO_ASSERT(write(out, buf, n) == n);
	close(in);
	close(out);

	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		execl(copy, copy, "--exe-test", g_dir, (char *)NULL);
		_exit(127);
	}
	CHECK(collect(pid, &status, 20000) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		printf("  (exe test exit status 0x%x)\n", status);
	printf("  a worker restarted after its executable was deleted and replaced still runs the original image\n");
}

/***********************************************************************
 * P5, P6: clean shutdown -- GOODBYE reaches the workers, which exit 0 by themselves
 ***********************************************************************/

static bool read_exitlog(unsigned int i, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;
	bool ok;

	snprintf(path, sizeof(path), "%s/exit%u", g_dir, i);
	f = fopen(path, "r");
	if (!f)
		return false;
	ok = fgets(buf, len, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

struct shutdown_result {
	pid_t pids[2];
	unsigned long stop_ms;	/*!< how long aworker_pool_stop() took */
};

/* Fork a main process with two workers (worker i gets "exitlog=..." plus extra[i] in its config) that shuts down
 * exactly like osmo_bsc_main.c: a shutdown request (SIGTERM), the event loop until osmo_select_shutdown_done() --
 * which, with nothing to write, ends at once -- then aworker_pool_stop(). */
static void run_shutdown(char **argv, const char *const *extra, struct shutdown_result *res)
{
	int pp[2], status;
	pid_t bsc;
	unsigned int i;

	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		char cfg[2][2 * PATH_MAX];
		const char *files[2];
		unsigned long t0;
		int st;

		close(pp[0]);
		for (i = 0; i < 2; i++) {
			snprintf(cfg[i], sizeof(cfg[i]), "exitlog=%s/exit%u\n%s", g_dir, i, extra[i]);
			files[i] = worker_cfg(i, i, cfg[i]);
		}
		pool_configure(2, files);
		aworker_pool_set_exec(argv[0], 1, argv);
		if (aworker_pool_start(ctx) < 0 || !run_until(all_ready, NULL, 10000))
			_exit(1);
		res->pids[0] = winfo(0).pid;
		res->pids[1] = winfo(1).pid;

		osmo_select_shutdown_request();
		while (!osmo_select_shutdown_done())
			osmo_select_main_ctx(0);
		t0 = now_ms();
		aworker_pool_stop();
		res->stop_ms = now_ms() - t0;
		st = write(pp[1], res, sizeof(*res)) == sizeof(*res) ? 0 : 1;
		_exit(st);
	}
	close(pp[1]);
	CHECK(read(pp[0], res, sizeof(*res)) == sizeof(*res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 10000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Worker i received GOODBYE (and so exited 0, the fake worker's only way out after it) and was never sent SIGTERM;
 * it is gone, collected by its main process. */
static void check_goodbye_no_sigterm(const struct shutdown_result *res, unsigned int i)
{
	char how[32] = {}, path[PATH_MAX];
	struct stat stbuf;

	CHECK(read_exitlog(i, how, sizeof(how)));
	CHECK(!strcmp(how, "goodbye"));
	snprintf(path, sizeof(path), "%s/exit%u.term", g_dir, i);
	CHECK(stat(path, &stbuf) < 0);
	CHECK(res->pids[i] > 0 && pid_gone(res->pids[i]));
}

static void clear_exitlogs(void)
{
	char path[PATH_MAX];
	unsigned int i;

	for (i = 0; i < 2; i++) {
		snprintf(path, sizeof(path), "%s/exit%u", g_dir, i);
		unlink(path);
		snprintf(path, sizeof(path), "%s/exit%u.term", g_dir, i);
		unlink(path);
	}
}

static void test_p5_clean_shutdown(char **argv)
{
	const char *extra[2] = { "", "" };
	struct shutdown_result res = {};
	unsigned int i;

	printf("P5: osmo-bsc shuts down; GOODBYE reaches every worker, which exits 0 by itself\n");
	clear_exitlogs();
	run_shutdown(argv, extra, &res);
	for (i = 0; i < 2; i++)
		check_goodbye_no_sigterm(&res, i);
	printf("  each worker received GOODBYE although the event loop had already ended, and exited 0: no EOF, no"
	       " SIGTERM\n");
	CHECK(res.stop_ms < AWORKER_TERM_GRACE_MS);
	printf("  aworker_pool_stop() returned well within the grace period\n");
}

static void test_p6_stop_waits_for_goodbye(char **argv)
{
	/* Both workers run on for 300 ms after GOODBYE (like a worker that tears down its SCTP associations). */
	const char *extra[2] = { "linger=300\n", "linger=300\n" };
	struct shutdown_result res = {};
	unsigned int i;

	printf("P6: the workers take a while to exit after GOODBYE\n");
	clear_exitlogs();
	run_shutdown(argv, extra, &res);
	for (i = 0; i < 2; i++)
		check_goodbye_no_sigterm(&res, i);
	CHECK(res.stop_ms >= 250 && res.stop_ms < AWORKER_TERM_GRACE_MS);
	printf("  aworker_pool_stop() waited for them to exit by themselves (no SIGTERM), well within the grace"
	       " period\n");
}

/***********************************************************************
 * P7: a worker restart is due when the pool is stopped
 ***********************************************************************/

static bool w0_restarting(void *data)
{
	return winfo(0).state == AWORKER_ST_RESTARTING;
}

static bool g_p7_control_fired;

static void p7_control_cb(void *data)
{
	g_p7_control_fired = true;
}

struct p7_result {
	bool restarting;	/*!< worker 0 was RESTARTING, with its restart overdue, when the pool was stopped */
	bool flush_ran;		/*!< worker 1's GOODBYE was pending, so aworker_pool_stop() polled the event loop */
	bool control_fired;	/*!< a timer due at the same time as the restart fired during aworker_pool_stop() */
	unsigned int forks;	/*!< fork() calls during aworker_pool_stop() */
};

static void test_p7_no_restart_while_stopping(char **argv)
{
	const char *extra[2] = { "", "" };
	struct p7_result res = {};
	int pp[2], status;
	pid_t bsc;

	printf("P7: a worker restart is due when the pool is stopped; no replacement is started\n");
	clear_exitlogs();
	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		struct osmo_timer_list control;
		char cfg[2][2 * PATH_MAX];
		const char *files[2];
		unsigned int i, forks0;
		int st;

		close(pp[0]);
		for (i = 0; i < 2; i++) {
			snprintf(cfg[i], sizeof(cfg[i]), "exitlog=%s/exit%u\n%s", g_dir, i, extra[i]);
			files[i] = worker_cfg(i, i, cfg[i]);
		}
		pool_configure(2, files);	/* restart-delay 1 s */
		aworker_pool_set_exec(argv[0], 1, argv);
		if (aworker_pool_start(ctx) < 0 || !run_until(all_ready, NULL, 10000))
			_exit(1);

		/* Lose worker 0: once collected, its restart is scheduled restart-delay (1 s) later. */
		kill(winfo(0).pid, SIGKILL);
		if (!run_until(w0_restarting, NULL, 5000))
			_exit(1);
		/* A control timer due at the same time (scheduled just after, so it expires just after). */
		osmo_timer_setup(&control, p7_control_cb, NULL);
		osmo_timer_schedule(&control, 1, 0);
		/* Let both become overdue without running the event loop: the next poll that runs timers fires
		 * them. No shutdown request, so libosmocore does run timers (it stops doing so after a request). */
		usleep(1100 * 1000);
		res.restarting = winfo(0).state == AWORKER_ST_RESTARTING && winfo(0).pid == 0;
		res.flush_ran = winfo(1).state == AWORKER_ST_READY;	/* its GOODBYE will be pending */

		forks0 = g_forks;
		aworker_pool_stop();
		res.forks = g_forks - forks0;
		res.control_fired = g_p7_control_fired;
		osmo_timer_del(&control);
		st = write(pp[1], &res, sizeof(res)) == sizeof(res) ? 0 : 1;
		_exit(st);
	}
	close(pp[1]);
	CHECK(read(pp[0], &res, sizeof(res)) == sizeof(res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 10000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	CHECK(res.restarting && res.flush_ran);
	printf("  worker 0 was RESTARTING with its restart overdue; worker 1 READY, so stop polled the event loop\n");
	CHECK(res.control_fired);
	printf("  a timer due at the same time as the restart fired while aworker_pool_stop() ran\n");
	CHECK(res.forks == 0);
	printf("  yet aworker_pool_stop() started no process\n");
}

/***********************************************************************
 * main
 ***********************************************************************/

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--aworker-fd"))
			return fake_worker_main(argc, argv);
	}
	if (argc == 3 && !strcmp(argv[1], "--exe-test")) {
		osmo_strlcpy(g_dir, argv[2], sizeof(g_dir));
		return exe_test_main(argv);
	}

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_proc.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	/* Orphans (P2, P3) are re-parented to us, so that we can see how they end. */
	OSMO_ASSERT(prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) == 0);
	bsc_init();
	setvbuf(stdout, NULL, _IOLBF, 0);

	test_p1_isolation(argv);
	test_p2_main_killed(argv);
	test_p3_pdeathsig_race(argv);
	test_p4_exe_replaced();
	test_p5_clean_shutdown(argv);
	test_p6_stop_waits_for_goodbye(argv);
	test_p7_no_restart_while_stopping(argv);

	if (!failures) {
		char cmd[PATH_MAX + 16];
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

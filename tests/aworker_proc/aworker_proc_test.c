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
	bool exit_config;		/*!< behave like a worker that cannot use its config file */
	bool no_cs7;			/*!< never send CS7_INFO: the worker never becomes READY */
	bool cs7_on_usr2;		/*!< send CS7_INFO only once SIGUSR2 arrives: READY when the test says so */
	volatile sig_atomic_t usr2;
	struct osmo_timer_list exit_timer;
	bool got_goodbye;
	struct aworker_ipc_link *link;
	volatile sig_atomic_t sever;
	unsigned long beats;
	int hb_fd;
	struct osmo_timer_list timer;
} fw;

static void fw_send_cs7(struct aworker_ipc_link *link)
{
	struct aworker_ipc_cs7_info info = {
		.count = 1,
		.e[0] = { .cs7_id = fw.cs7, .primary_pc = 185, .asp_proto = OSMO_SS7_ASP_PROT_M3UA, .num_as = 1 },
	};
	struct msgb *rec;

	OSMO_ASSERT(aworker_ipc_enc_cs7_info(ctx, &rec, &info) == 0);
	aworker_ipc_link_send(link, rec, NULL);
}

static void fw_ready_cb(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	if (fw.no_cs7 || fw.cs7_on_usr2)
		return;
	fw_send_cs7(link);
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

static void fw_sigusr2(int sig)
{
	fw.usr2 = 1;
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
	if (fw.usr2 == 1 && fw.link) {
		fw.usr2 = 2;
		fw_send_cs7(fw.link);
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
		else if (!strcmp(line, "exit_config=1"))
			fw.exit_config = true;
		else if (!strcmp(line, "no_cs7=1"))
			fw.no_cs7 = true;
		else if (!strcmp(line, "cs7_on_usr2=1"))
			fw.cs7_on_usr2 = true;
	}
	fclose(f);
	if (fw.exit_config)
		return AWORKER_EXIT_CONFIG;

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
	signal(SIGUSR2, fw_sigusr2);
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

static bool never_p8(void *data)
{
	return false;
}

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
		struct osmo_timer_list control = {};	/* osmo_timer_setup() does not clear it */
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
 * P8: a worker that cannot use its config file is FAILED; only an operator reset starts it again
 ***********************************************************************/

static bool p8_w0_ready_w1_failed(void *data)
{
	return winfo(0).state == AWORKER_ST_READY && winfo(1).state == AWORKER_ST_FAILED;
}

static bool w1_ready(void *data)
{
	return winfo(1).state == AWORKER_ST_READY;
}

struct p8_result {
	bool reached;		/*!< worker 0 READY and worker 1 FAILED */
	bool recorded;		/*!< reason config, detail names exit 66 and the file, time set */
	bool stayed_failed;	/*!< still FAILED after restart-delay and startup-timeout passed, never restarted */
	unsigned int forks_while_failed;
	bool w0_untouched;	/*!< worker 0: READY with the same process from start to end */
	bool no_shutdown;	/*!< osmo-bsc (this process) was never asked to shut down */
	bool reset_ready_refused; /*!< resetting the READY worker 0: -EBUSY, nothing changed */
	bool reset_ok;		/*!< resetting worker 1: 0, one fork, READY, failure forgotten */
	unsigned int forks_on_reset;
	bool reset_again_refused;
};

static void test_p8_failed_worker(char **argv)
{
	struct p8_result res = {};
	int pp[2], status;
	pid_t bsc;

	printf("P8: a worker that cannot use its config file is FAILED; the others and osmo-bsc go on\n");
	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		const char *files[2];
		struct aworker_pool_worker_info i0, i1;
		pid_t p0;
		unsigned int forks0;
		int st;

		close(pp[0]);
		files[0] = worker_cfg(0, 0, NULL);
		files[1] = worker_cfg(1, 1, "exit_config=1\n");
		pool_configure(2, files);	/* restart-delay 1 s */
		aworker_pool_cfg()->startup_timeout = 2;
		aworker_pool_set_exec(argv[0], 1, argv);
		if (aworker_pool_start(ctx) < 0)
			_exit(1);
		res.reached = run_until(p8_w0_ready_w1_failed, NULL, 10000);
		p0 = winfo(0).pid;
		i1 = winfo(1);
		res.recorded = i1.fail_reason == AWORKER_FAIL_CONFIG && i1.failed_at != 0 && i1.fail_detail
			       && strstr(i1.fail_detail, "exited with status 66") && strstr(i1.fail_detail, files[1]);

		/* Past restart-delay (1 s) and startup-timeout (2 s): neither may start it again. */
		forks0 = g_forks;
		run_until(never_p8, NULL, 3000);
		i1 = winfo(1);
		res.forks_while_failed = g_forks - forks0;
		res.stayed_failed = i1.state == AWORKER_ST_FAILED && i1.starts == 1 && !res.forks_while_failed;

		/* Resetting a worker that is not FAILED changes nothing. */
		res.reset_ready_refused = aworker_pool_reset_worker(0) == -EBUSY && winfo(0).state == AWORKER_ST_READY
					  && winfo(0).pid == p0;

		/* The operator fixes the file and resets worker 1: only worker 1 is started. */
		worker_cfg(1, 1, NULL);
		forks0 = g_forks;
		st = aworker_pool_reset_worker(1);
		res.reset_ok = st == 0 && run_until(w1_ready, NULL, 10000);
		res.forks_on_reset = g_forks - forks0;
		i1 = winfo(1);
		res.reset_ok = res.reset_ok && i1.fail_reason == AWORKER_FAIL_NONE && i1.failed_at == 0
			       && !i1.fail_detail[0] && i1.starts == 2;
		res.reset_again_refused = aworker_pool_reset_worker(1) == -EBUSY;

		i0 = winfo(0);
		res.w0_untouched = i0.state == AWORKER_ST_READY && i0.pid == p0 && i0.starts == 1;
		res.no_shutdown = !osmo_select_shutdown_requested() && !aworker_pool_shutdown_was_fatal();
		aworker_pool_stop();
		st = write(pp[1], &res, sizeof(res)) == sizeof(res) ? 0 : 1;
		_exit(st);
	}
	close(pp[1]);
	CHECK(read(pp[0], &res, sizeof(res)) == sizeof(res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 30000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	CHECK(res.reached && res.recorded);
	printf("  worker 1 exited 66 and is FAILED (reason config, with the file and the time); worker 0 READY\n");
	CHECK(res.stayed_failed);
	CHECK(res.forks_while_failed == 0);
	printf("  it stays FAILED past restart-delay and startup-timeout: no process started\n");
	CHECK(res.reset_ready_refused);
	printf("  resetting a worker that is not FAILED is refused and changes nothing\n");
	CHECK(res.reset_ok);
	CHECK(res.forks_on_reset == 1);
	CHECK(res.reset_again_refused);
	printf("  after the file is fixed, a reset starts exactly that worker once; it becomes READY\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
	printf("  worker 0 kept its process throughout, and osmo-bsc was never asked to shut down\n");
}

/***********************************************************************
 * P9: a worker claiming a cs7 instance its file did not configure at start is FAILED (ownership)
 ***********************************************************************/

struct p9_result {
	bool reached;		/*!< worker 0 READY and worker 1 FAILED */
	bool recorded;		/*!< reason ownership, detail names the instance */
	bool stayed_failed;	/*!< past restart-delay and startup-timeout: still FAILED, never restarted */
	unsigned int forks_while_failed;
	bool reset_ok;		/*!< after the worker is fixed, a reset makes it READY with the failure forgotten */
	unsigned int forks_on_reset;
	bool w0_untouched;
	bool no_shutdown;
};

static void test_p9_ownership_failed(char **argv)
{
	/* The pool reads the 'cs7 instance' sections (the expected ownership); the fake worker reports 'cs7='. */
	const char *own0 = "cs7 instance 0\n asp asp-0 2905 0 m3ua\n";
	const char *own1 = "cs7 instance 1\n asp asp-1 2905 0 m3ua\n";
	struct p9_result res = {};
	int pp[2], status;
	pid_t bsc;

	printf("P9: a worker claiming a cs7 instance its file did not configure at start is FAILED\n");
	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		const char *files[2];
		struct aworker_pool_worker_info i0, i1;
		unsigned int forks0;
		pid_t p0;
		int st;

		close(pp[0]);
		files[0] = worker_cfg(0, 0, own0);
		files[1] = worker_cfg(1, 2, own1);	/* configures instance 1, reports instance 2 */
		pool_configure(2, files);		/* restart-delay 1 s */
		aworker_pool_cfg()->startup_timeout = 2;
		aworker_pool_set_exec(argv[0], 1, argv);
		if (aworker_pool_start(ctx) < 0)
			_exit(1);
		res.reached = run_until(p8_w0_ready_w1_failed, NULL, 10000);
		p0 = winfo(0).pid;
		i1 = winfo(1);
		res.recorded = i1.fail_reason == AWORKER_FAIL_OWNERSHIP && i1.failed_at != 0 && i1.fail_detail
			       && strstr(i1.fail_detail, "claims cs7 instance 2, which its config file did not"
					 " configure at start");

		forks0 = g_forks;
		run_until(never_p8, NULL, 3000);
		i1 = winfo(1);
		res.forks_while_failed = g_forks - forks0;
		res.stayed_failed = i1.state == AWORKER_ST_FAILED && i1.starts == 1 && !res.forks_while_failed;

		/* The worker is put right (it reports instance 1 again), then reset. */
		worker_cfg(1, 1, own1);
		forks0 = g_forks;
		res.reset_ok = aworker_pool_reset_worker(1) == 0 && run_until(w1_ready, NULL, 10000);
		res.forks_on_reset = g_forks - forks0;
		i1 = winfo(1);
		res.reset_ok = res.reset_ok && i1.fail_reason == AWORKER_FAIL_NONE && !i1.fail_detail[0];

		i0 = winfo(0);
		res.w0_untouched = i0.state == AWORKER_ST_READY && i0.pid == p0 && i0.starts == 1;
		res.no_shutdown = !osmo_select_shutdown_requested() && !aworker_pool_shutdown_was_fatal();
		aworker_pool_stop();
		st = write(pp[1], &res, sizeof(res)) == sizeof(res) ? 0 : 1;
		_exit(st);
	}
	close(pp[1]);
	CHECK(read(pp[0], &res, sizeof(res)) == sizeof(res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 30000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

	CHECK(res.reached && res.recorded);
	printf("  worker 1 reported instance 2, its file configured 1: FAILED (reason ownership); worker 0 READY\n");
	CHECK(res.stayed_failed);
	CHECK(res.forks_while_failed == 0);
	printf("  it stays FAILED past restart-delay and startup-timeout: no process started\n");
	CHECK(res.reset_ok);
	CHECK(res.forks_on_reset == 1);
	printf("  once it reports what its file configured, a reset starts it once and it becomes READY\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
	printf("  worker 0 kept its process throughout, and osmo-bsc was never asked to shut down\n");
}


/***********************************************************************
 * Restart backoff (P10-P12), with a fake clock
 *
 * In the osmo-bsc process of these tests, libosmocore's clocks are overridden: timers (osmo_gettimeofday()) and
 * the pool's CLOCK_MONOTONIC (osmo_clock_gettime()) only move when the test moves them. Everything else -- the
 * workers' fork/exec, IPC handshake, CS7_INFO and process exit -- happens in real time while the fake clock stands
 * still, so a delay is checked to the millisecond: no start 1 ms before it, exactly one start at it.
 ***********************************************************************/

static unsigned long g_fake_ms;	/*!< fake time since fake_clock_on() */
static unsigned int g_startup_timeout_default;	/*!< aworker_pool_cfg()->startup_timeout before any test set it */

static void fake_clock_on(void)
{
	g_fake_ms = 0;
	osmo_gettimeofday_override = true;
	gettimeofday(&osmo_gettimeofday_override_time, NULL);
	osmo_clock_override_enable(CLOCK_MONOTONIC, true);
	clock_gettime(CLOCK_MONOTONIC, osmo_clock_override_gettimespec(CLOCK_MONOTONIC));
}

/* Before aworker_pool_stop(): its bounded waits measure real time. */
static void fake_clock_off(void)
{
	osmo_gettimeofday_override = false;
	osmo_clock_override_enable(CLOCK_MONOTONIC, false);
}

/* A few non-blocking event loop iterations: expired timers fire, pending I/O is handled. */
static void pump(void)
{
	int i;
	for (i = 0; i < 20; i++) {
		if (!osmo_select_main_ctx(1))
			usleep(500);
	}
}

static void fake_advance_ms(unsigned long ms)
{
	g_fake_ms += ms;
	osmo_gettimeofday_override_add(ms / 1000, (ms % 1000) * 1000);
	osmo_clock_override_add(CLOCK_MONOTONIC, ms / 1000, (ms % 1000) * 1000000);
	pump();
}

static bool is_zombie(pid_t pid)
{
	char path[64], buf[256], *p;
	FILE *f;
	bool z = false;

	snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
	f = fopen(path, "r");
	if (!f)
		return false;
	if (fgets(buf, sizeof(buf), f) && (p = strrchr(buf, ')')))
		z = p[1] == ' ' && p[2] == 'Z';
	fclose(f);
	return z;
}

static bool w_restarting_or_failed(void *data)
{
	enum aworker_state st = winfo(*(unsigned int *)data).state;
	return st == AWORKER_ST_RESTARTING || st == AWORKER_ST_FAILED;
}

static bool w_ready(void *data)
{
	return winfo(*(unsigned int *)data).state == AWORKER_ST_READY;
}

/* Crash worker i (SIGKILL) and let the pool handle it. The test first waits until the process is a zombie, so that
 * the pool collects it at once, at the current fake time (no SIGTERM, no reap timer). \returns false if the pool
 * did not decide on a restart (or FAILED) in time. */
static bool crash(unsigned int i)
{
	pid_t pid = winfo(i).pid;
	unsigned long end = now_ms() + 5000;

	if (pid <= 0 || kill(pid, SIGKILL))
		return false;
	while (!is_zombie(pid) && now_ms() < end)
		usleep(1000);
	return run_until(w_restarting_or_failed, &i, 5000);
}

/* Worker i is RESTARTING with a delay of delay_s: no start up to 1 ms before it, exactly one start at it, and the
 * new process becomes READY (in real time; the fake clock stands still meanwhile). */
static bool restarts_after(unsigned int i, unsigned int delay_s)
{
	unsigned int forks0 = g_forks;

	if (delay_s) {
		fake_advance_ms(delay_s * 1000UL - 1);
		if (g_forks != forks0 || winfo(i).state != AWORKER_ST_RESTARTING)
			return false;
		fake_advance_ms(1);
	} else {
		pump();
	}
	return g_forks == forks0 + 1 && run_until(w_ready, &i, 10000) && g_forks == forks0 + 1;
}

/* P13-P15 set their own restart limit before backoff_pool_start(); 0: the default. */
static unsigned int g_bo_max_restarts, g_bo_window;

/* Start a two-worker pool on the fake clock; worker 1 is the one that crashes. \returns false unless both are READY. */
static bool backoff_pool_start(char **argv, const char *extra1, unsigned int startup_timeout)
{
	const char *files[2];

	files[0] = worker_cfg(0, 0, NULL);
	files[1] = worker_cfg(1, 1, extra1);
	pool_configure(2, files);
	/* The approved defaults; pool_configure() sets the Phase 1 test values. */
	aworker_pool_cfg()->restart_delay = AWORKER_RESTART_DELAY_DEFAULT;
	aworker_pool_cfg()->restart_delay_max = AWORKER_RESTART_DELAY_MAX_DEFAULT;
	aworker_pool_cfg()->stable_time = AWORKER_STABLE_TIME_DEFAULT;
	aworker_pool_cfg()->max_restarts = g_bo_max_restarts ? : AWORKER_MAX_RESTARTS_DEFAULT;
	aworker_pool_cfg()->restart_window = g_bo_window ? : AWORKER_RESTART_WINDOW_DEFAULT;
	aworker_pool_cfg()->startup_timeout = startup_timeout;
	aworker_pool_set_exec(argv[0], 1, argv);
	fake_clock_on();
	return aworker_pool_start(ctx) == 0 && run_until(all_ready, NULL, 10000);
}

#define BACKOFF_MAX_STEPS 8

struct backoff_result {
	bool started;
	unsigned int n;					/*!< losses */
	unsigned int delay[BACKOFF_MAX_STEPS];		/*!< restart delay the pool chose after each loss */
	unsigned int step[BACKOFF_MAX_STEPS];		/*!< backoff_step right after each loss */
	bool on_time[BACKOFF_MAX_STEPS];		/*!< restarted exactly after that delay, then READY */
	unsigned int step_when_stable;			/*!< P11: backoff_step reported while READY >= stable-time */
	unsigned int step_before_stable;		/*!< P11: ... and 1 ms before */
	bool w0_untouched;				/*!< worker 0 READY with its first process throughout */
	bool no_shutdown;
	/* restart limit (P13-P15) */
	bool failed;			/*!< the worker FAILED with reason restart-limit and the expected detail */
	unsigned int in_window;		/*!< its restarts_in_window then */
	bool stayed_failed;		/*!< still FAILED, no process started, long after */
	unsigned int forks_while_failed;
	bool reset_ok;			/*!< reset: one start, READY, the record and the backoff forgotten */
	bool other_ok;			/*!< P14: the other worker's own record decides */
	unsigned int in_window_later;	/*!< P14: worker 0's restarts_in_window once its restart is S s old */
	unsigned int in_window_stable;	/*!< P16: restarts_in_window right after the loss that followed a stable period */
	unsigned int step_stable;	/*!< P16: backoff_step right after that loss */
	bool stop_ok;			/*!< P15: a loss while stopping counts nothing */
};

/* Lose worker 1 after it was READY for up_ms (fake time), record the restart decision, and let it restart. */
static void backoff_round(struct backoff_result *res, unsigned long up_ms)
{
	struct aworker_pool_worker_info i1;
	unsigned int n = res->n;

	if (n >= BACKOFF_MAX_STEPS)
		return;
	if (up_ms)
		fake_advance_ms(up_ms);
	if (!crash(1))
		return;
	i1 = winfo(1);
	res->delay[n] = i1.restart_delay;
	res->step[n] = i1.backoff_step;
	res->n++;
	if (i1.state == AWORKER_ST_RESTARTING)
		res->on_time[n] = restarts_after(1, i1.restart_delay);
}

/* Run fn in a forked osmo-bsc process (on the fake clock) and return what it found. */
static void backoff_run(char **argv, void (*fn)(char **argv, struct backoff_result *res), struct backoff_result *res)
{
	int pp[2], status;
	pid_t bsc;

	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		close(pp[0]);
		memset(res, 0, sizeof(*res));
		fn(argv, res);
		/* starts == 1: the process it got at pool start is still the one (P15 checks before its stop) */
		if (aworker_pool_num_workers())
			res->w0_untouched = res->started && winfo(0).state == AWORKER_ST_READY
					    && winfo(0).starts == 1 && winfo(0).pid > 0;
		res->no_shutdown = !osmo_select_shutdown_requested() && !aworker_pool_shutdown_was_fatal();
		fake_clock_off();
		aworker_pool_stop();
		_exit(write(pp[1], res, sizeof(*res)) == sizeof(*res) ? 0 : 1);
	}
	close(pp[1]);
	CHECK(read(pp[0], res, sizeof(*res)) == sizeof(*res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 60000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(res->started);
}

static bool check_rounds(const struct backoff_result *res, const unsigned int *delays, unsigned int n)
{
	unsigned int i;
	bool ok = res->n == n;

	for (i = 0; i < n && i < res->n; i++) {
		if (res->delay[i] != delays[i] || !res->on_time[i]) {
			printf("  loss %u: restart delay %u s (expected %u s), restarted on time: %s\n", i + 1,
			       res->delay[i], delays[i], res->on_time[i] ? "yes" : "no");
			ok = false;
		}
	}
	return ok;
}

/* P10: worker 1 is lost every time 59 s after it became READY -- 1 s short of stable-time, so the backoff never
 * starts over. Losses are 59 s + delay apart, so at most 3 earlier restarts are ever within 300 s and the restart
 * limit (2a-4b) is never reached: the delays double from restart-delay and stay at restart-delay-max. */
static void p10_fn(char **argv, struct backoff_result *res)
{
	unsigned int i;

	res->started = backoff_pool_start(argv, NULL, 600);	/* startup timer out of the way */
	if (!res->started)
		return;
	backoff_round(res, 0);
	for (i = 1; i < 6; i++)
		backoff_round(res, 59000);
}

static void test_p10_backoff(char **argv)
{
	static const unsigned int delays[] = { 5, 10, 20, 40, 60, 60 };
	static const unsigned int steps[] = { 1, 2, 3, 4, 5, 6 };
	struct backoff_result res;
	unsigned int i;
	bool steps_ok = true;

	printf("P10: a worker lost again and again is restarted with an exponential backoff, capped\n");
	backoff_run(argv, p10_fn, &res);
	CHECK(check_rounds(&res, delays, ARRAY_SIZE(delays)));
	for (i = 0; i < ARRAY_SIZE(steps); i++)
		steps_ok = steps_ok && res.step[i] == steps[i];
	CHECK(steps_ok);
	printf("  the first restart waits restart-delay (5 s), each further loss doubles it: 5, 10, 20, 40 s\n");
	printf("  restart-delay-max (60 s) caps it: 60, 60 s; no start 1 ms before a delay, exactly one at it\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
	printf("  worker 0 kept its process and stayed READY, and osmo-bsc was never asked to shut down\n");
}

/* P11: READY for stable-time before the loss starts the backoff over; 1 ms less does not. */
static void p11_fn(char **argv, struct backoff_result *res)
{
	res->started = backoff_pool_start(argv, NULL, 600);	/* startup timer out of the way */
	if (!res->started)
		return;
	backoff_round(res, 0);			/* 5 s */
	backoff_round(res, 59999);		/* READY 1 ms short of stable-time: 10 s */
	fake_advance_ms(59999);
	res->step_before_stable = winfo(1).backoff_step;
	fake_advance_ms(1);
	res->step_when_stable = winfo(1).backoff_step;
	backoff_round(res, 0);			/* READY exactly stable-time: back to 5 s */
	backoff_round(res, 0);			/* lost right away again: 10 s */
}

static void test_p11_stable_time(char **argv)
{
	static const unsigned int delays[] = { 5, 10, 5, 10 };
	struct backoff_result res;

	printf("P11: a worker that was READY for stable-time before it was lost starts the backoff over\n");
	backoff_run(argv, p11_fn, &res);
	CHECK(check_rounds(&res, delays, ARRAY_SIZE(delays)));
	CHECK(res.step_before_stable == 2 && res.step_when_stable == 0);
	printf("  READY 59.999 s, then lost: the delay still doubles (5 -> 10 s)\n");
	printf("  READY 60 s (stable-time), then lost: back to restart-delay (5 s); lost again at once: 10 s\n");
	printf("  the reported backoff step drops to 0 exactly when the worker has been READY for stable-time\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
}

/* P12: a process that never becomes READY is not stable, however long it runs: the startup timeout that ends it is
 * an unplanned loss like any other, and the backoff goes on. */
static bool w1_starting(void *data)
{
	return winfo(1).state == AWORKER_ST_STARTING;
}

static void p12_fn(char **argv, struct backoff_result *res)
{
	struct aworker_pool_worker_info i1;
	unsigned int forks0;
	unsigned long end;

	/* Fake time 0: the pool starts. startup-timeout 65 s per start (2a-5): the process started at 15 s, which never
	 * becomes READY, times out at 80 s. */
	res->started = backoff_pool_start(argv, NULL, 65);
	if (!res->started)
		return;
	backoff_round(res, 0);			/* lost at 0 s: 5 s; restarted at 5 s, READY */
	worker_cfg(1, 1, "no_cs7=1\n");		/* its next process never reports its cs7 instances */
	if (!crash(1))				/* lost at 5 s: 10 s */
		return;
	i1 = winfo(1);
	res->delay[res->n] = i1.restart_delay;
	res->step[res->n] = i1.backoff_step;
	forks0 = g_forks;
	fake_advance_ms(i1.restart_delay * 1000UL);	/* restarted at 15 s; STARTING from now on */
	res->on_time[res->n] = g_forks == forks0 + 1 && run_until(w1_starting, NULL, 5000);
	res->n++;

	/* 80 s: STARTING for 65 s -- longer than stable-time -- when the startup timeout ends it. */
	fake_advance_ms(65000);
	end = now_ms() + 5000;
	while (winfo(1).state != AWORKER_ST_RESTARTING && now_ms() < end)
		fake_advance_ms(10);	/* the process is SIGTERMed and collected by the reap timer */
	i1 = winfo(1);
	if (i1.state != AWORKER_ST_RESTARTING)
		return;
	res->delay[res->n] = i1.restart_delay;
	res->step[res->n] = i1.backoff_step;
	worker_cfg(1, 1, NULL);
	res->on_time[res->n] = restarts_after(1, i1.restart_delay);
	res->n++;
}

static void test_p12_startup_not_stable(char **argv)
{
	static const unsigned int delays[] = { 5, 10, 20 };
	struct backoff_result res;

	printf("P12: time spent starting is not stable time: a startup timeout continues the backoff\n");
	backoff_run(argv, p12_fn, &res);
	CHECK(check_rounds(&res, delays, ARRAY_SIZE(delays)));
	CHECK(res.n == 3 && res.step[2] == 3);
	printf("  a process that never became READY ran 65 s (more than stable-time) until the startup timeout:\n");
	printf("  that loss is the third in a row (5, 10, 20 s), not a fresh start\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
}


/***********************************************************************
 * Restart limit (P13-P15), 'max-restarts N window S' as approved (plan §11.2): an unplanned loss that would be the
 * N-th restart within S seconds of the oldest recorded one makes the worker FAILED -- at most N-1 restarts.
 ***********************************************************************/

static bool limit_failed(unsigned int i, const char *detail_part)
{
	struct aworker_pool_worker_info inf = winfo(i);
	return inf.state == AWORKER_ST_FAILED && inf.fail_reason == AWORKER_FAIL_RESTART_LIMIT && inf.failed_at
	       && inf.fail_detail && strstr(inf.fail_detail, detail_part);
}

/* P13: the defaults (5 in 300 s); worker 1 lost as soon as it is READY. */
static void p13_fn(char **argv, struct backoff_result *res)
{
	struct aworker_pool_worker_info i1;
	unsigned int i, forks0;

	res->started = backoff_pool_start(argv, NULL, 250);	/* the startup timer fires while it is FAILED */
	if (!res->started)
		return;
	for (i = 0; i < 4; i++)
		backoff_round(res, 0);		/* restarts 1-4: 5, 10, 20, 40 s; the 4th at 75 s */
	if (!crash(1))				/* would be restart 5 within 300 s of restart 1 */
		return;
	i1 = winfo(1);
	res->in_window = i1.restarts_in_window;
	res->failed = limit_failed(1, "lost again after 4 restarts within 300 s (the first 75 s ago): restart 5"
				      " would reach 'max-restarts 5 window 300'");

	/* Never restarted automatically: neither by a restart timer nor by the startup timer (250 s). */
	forks0 = g_forks;
	fake_advance_ms(200 * 1000);
	pump();
	res->forks_while_failed = g_forks - forks0;
	res->stayed_failed = limit_failed(1, "max-restarts 5 window 300") && !res->forks_while_failed;

	/* 275 s: all 4 recorded restarts are still within 300 s. The operator resets it: one start, READY, a fresh
	 * record and backoff. */
	forks0 = g_forks;
	i = 1;
	res->reset_ok = aworker_pool_reset_worker(1) == 0 && run_until(w_ready, &i, 10000) && g_forks == forks0 + 1;
	i1 = winfo(1);
	res->reset_ok = res->reset_ok && i1.fail_reason == AWORKER_FAIL_NONE && !i1.restarts_in_window
			&& !i1.backoff_step;
	backoff_round(res, 0);			/* lost again: restart-delay 5 s, restart 1 of a new window */
	res->reset_ok = res->reset_ok && winfo(1).restarts_in_window == 1;
}

static void test_p13_restart_limit(char **argv)
{
	static const unsigned int delays[] = { 5, 10, 20, 40, 5 };
	struct backoff_result res;

	printf("P13: a worker lost again and again is FAILED once its restarts reach 'max-restarts 5 window 300'\n");
	backoff_run(argv, p13_fn, &res);
	CHECK(check_rounds(&res, delays, ARRAY_SIZE(delays)));
	CHECK(res.failed);
	CHECK(res.in_window == 4);
	printf("  4 restarts (5, 10, 20, 40 s); the loss that would be the 5th restart within 300 s makes it FAILED\n");
	printf("  (reason restart-limit; the detail names the restarts, the window and the limit)\n");
	CHECK(res.stayed_failed);
	CHECK(res.forks_while_failed == 0);
	printf("  it stays FAILED: no process started in the 200 s that follow (the startup timer fires meanwhile)\n");
	CHECK(res.reset_ok);
	printf("  a reset starts it once with the record and the backoff forgotten: lost again, restarted after 5 s\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
	printf("  worker 0 kept its process and stayed READY, and osmo-bsc was never asked to shut down\n");
}

/* P14: 'max-restarts 2 window 300': the second restart within 300 s is refused. Both workers are lost at 0 s and
 * restarted; worker 0 is lost again at 299.999 s (FAILED), worker 1 at 300 s, when its first restart has left the
 * window (restarted). Each worker's own record decides. */
static void p14_fn(char **argv, struct backoff_result *res)
{
	struct aworker_pool_worker_info i1;
	unsigned int forks0;

	g_bo_max_restarts = 2;
	g_bo_window = 300;
	res->started = backoff_pool_start(argv, NULL, 600);
	if (!res->started)
		return;
	/* 0 s: both lost; each is restart 1 of its own window */
	if (!crash(0) || !crash(1) || winfo(0).state != AWORKER_ST_RESTARTING
	    || winfo(1).state != AWORKER_ST_RESTARTING)
		return;
	forks0 = g_forks;
	fake_advance_ms(4999);
	if (g_forks != forks0)
		return;
	fake_advance_ms(1);
	if (g_forks != forks0 + 2 || !run_until(all_ready, NULL, 10000))
		return;
	/* 299.999 s: worker 0's restart at 0 s is still within the window */
	fake_advance_ms(294999);
	if (!crash(0))
		return;
	res->failed = limit_failed(0, "lost again after 1 restarts within 300 s (the first 299 s ago): restart 2"
				      " would reach 'max-restarts 2 window 300'");
	res->in_window = winfo(0).restarts_in_window;
	/* 300 s: the restarts at 0 s have just left the window */
	fake_advance_ms(1);
	res->in_window_later = winfo(0).restarts_in_window;
	if (!crash(1))
		return;
	i1 = winfo(1);
	res->other_ok = i1.state == AWORKER_ST_RESTARTING && i1.restarts_in_window == 1 && i1.restart_delay == 5
			&& restarts_after(1, 5) && limit_failed(0, "max-restarts 2 window 300");
}

static void test_p14_restart_window(char **argv)
{
	struct backoff_result res;

	printf("P14: the restart window is per worker and slides: a restart counts while it is less than S s old\n");
	backoff_run(argv, p14_fn, &res);
	CHECK(res.started);
	CHECK(res.failed && res.in_window == 1);
	printf("  'max-restarts 2 window 300': worker 0, restarted at 0 s, lost again at 299.999 s: FAILED\n");
	CHECK(res.in_window_later == 0);
	printf("  at 300 s its status no longer counts that restart\n");
	CHECK(res.other_ok);
	printf("  worker 1, restarted at 0 s too, lost again at 300 s: that restart left the window, it is restarted\n");
}

/* P15: a planned stop is no unplanned loss. With 'max-restarts 1', a loss the pool handles while it runs makes the
 * worker FAILED at once; the same loss handled while the pool stops counts nothing. */
static void p15_running_fn(char **argv, struct backoff_result *res)
{
	g_bo_max_restarts = 1;
	res->started = backoff_pool_start(argv, NULL, 600);
	if (!res->started)
		return;
	res->failed = crash(1) && limit_failed(1, "lost: 'max-restarts 1 window 300' allows no automatic restart");
}

static void p15_stopping_fn(char **argv, struct backoff_result *res)
{
	char path[PATH_MAX], text[16384];
	struct log_target *t;
	unsigned long end;
	unsigned int forks0;
	pid_t pid;
	FILE *f;
	size_t n;

	g_bo_max_restarts = 1;
	res->started = backoff_pool_start(argv, NULL, 600);
	if (!res->started)
		return;
	res->w0_untouched = winfo(0).starts == 1;
	snprintf(path, sizeof(path), "%s/p15.log", g_dir);
	t = log_target_create_file(path);
	OSMO_ASSERT(t);
	log_set_all_filter(t, 1);
	log_set_category_filter(t, DAWORKER, 1, LOGL_DEBUG);
	log_set_print_filename2(t, LOG_FILENAME_NONE);
	log_add_target(t);

	/* Worker 1 dies; the pool has not handled it when osmo-bsc stops: it does so during aworker_pool_stop(). */
	pid = winfo(1).pid;
	kill(pid, SIGKILL);
	end = now_ms() + 5000;
	while (!is_zombie(pid) && now_ms() < end)
		usleep(1000);
	forks0 = g_forks;
	fake_clock_off();
	aworker_pool_stop();
	log_target_destroy(t);

	f = fopen(path, "r");
	n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
	text[n] = '\0';
	if (f)
		fclose(f);
	res->stop_ok = is_zombie(pid) == false && g_forks == forks0
		       && strstr(text, "SIGKILL") == NULL		/* it was simply collected */
		       && strstr(text, "restart-limit") == NULL && strstr(text, "FAILED") == NULL
		       && strstr(text, "restarting in") == NULL;
	if (!res->stop_ok)
		printf("%s", text);
}

static void test_p15_stop_not_counted(char **argv)
{
	struct backoff_result res;

	printf("P15: a planned stop never counts as an unplanned loss\n");
	backoff_run(argv, p15_running_fn, &res);
	CHECK(res.failed);
	printf("  'max-restarts 1': a worker lost while the pool runs is FAILED (restart-limit) at once\n");
	backoff_run(argv, p15_stopping_fn, &res);
	CHECK(res.stop_ok);
	CHECK(res.w0_untouched);
	printf("  the same loss handled while the pool stops: not counted, not FAILED, no restart, nothing started\n");
}

/* P16: stable-time resets only the backoff, never the restart record (plan §3, §8, §11.2). Worker 1, defaults:
 * lost at 0, 5 and 15 s right after READY (restarts after 5, 10, 20 s), then READY 60 s from 35 s and lost at 95 s:
 * the restart waits restart-delay again, but the restarts of 0, 5 and 15 s still count and the new one is added
 * (4 in the window). READY 60 s again, lost at 160 s: that would be restart 5 within 300 s of the one at 0 s, so
 * the worker is FAILED although both of its last losses followed a stable period. */
static void p16_fn(char **argv, struct backoff_result *res)
{
	struct aworker_pool_worker_info i1;

	res->started = backoff_pool_start(argv, NULL, 600);
	if (!res->started)
		return;
	backoff_round(res, 0);			/*   0 s: 5 s */
	backoff_round(res, 0);			/*   5 s: 10 s */
	backoff_round(res, 0);			/*  15 s: 20 s; READY at 35 s */
	backoff_round(res, 60000);		/*  95 s, READY 60 s: 5 s; READY at 100 s */
	i1 = winfo(1);
	res->in_window_stable = i1.restarts_in_window;
	res->step_stable = i1.backoff_step;
	fake_advance_ms(60000);			/* 160 s, READY 60 s */
	if (!crash(1))
		return;
	res->in_window = winfo(1).restarts_in_window;
	res->failed = limit_failed(1, "lost again after 4 restarts within 300 s (the first 160 s ago): restart 5"
				      " would reach 'max-restarts 5 window 300'");
}

static void test_p16_stable_keeps_record(char **argv)
{
	static const unsigned int delays[] = { 5, 10, 20, 5 };
	struct backoff_result res;

	printf("P16: stable-time resets the backoff, not the restart record\n");
	backoff_run(argv, p16_fn, &res);
	CHECK(check_rounds(&res, delays, ARRAY_SIZE(delays)));
	CHECK(res.step_stable == 1);
	printf("  lost at 0, 5, 15 s (5, 10, 20 s), then READY 60 s and lost: restarted after restart-delay (5 s)\n");
	CHECK(res.in_window_stable == 4);
	printf("  the restarts of 0, 5 and 15 s still count, and that restart is added: 4 within 300 s\n");
	CHECK(res.failed && res.in_window == 4);
	printf("  READY 60 s again, lost at 160 s: restart 5 within 300 s, so FAILED (restart-limit)\n");
	CHECK(res.w0_untouched);
	CHECK(res.no_shutdown);
}

/***********************************************************************
 * Startup timeout per start (P17-P19), on the fake clock; 'startup-timeout 60', the other settings default.
 ***********************************************************************/

/* Move the fake clock to t_ms (since fake_clock_on()); never backwards. */
static void fake_advance_to(unsigned long t_ms)
{
	if (t_ms > g_fake_ms)
		fake_advance_ms(t_ms - g_fake_ms);
	else
		pump();
}

/* Worker i was lost with its process still running (SIGTERM sent): let the reap timer collect it (fake time moves
 * in 10 ms steps) until the pool has decided. \returns true once RESTARTING or FAILED. */
static bool collected(unsigned int i)
{
	unsigned long end = now_ms() + 5000;

	while (!w_restarting_or_failed(&i) && now_ms() < end)
		fake_advance_ms(10);
	return w_restarting_or_failed(&i);
}

static bool w_is(unsigned int i, enum aworker_state st, pid_t pid)
{
	struct aworker_pool_worker_info inf = winfo(i);
	return inf.state == st && (!pid || inf.pid == pid);
}

/* Lose worker i at the current fake time (crash()), its config file is changed to extra first; then start its next
 * process after the restart delay. \returns the new process' pid (STARTING), or 0. */
static pid_t restart_with(unsigned int i, const char *extra)
{
	struct aworker_pool_worker_info inf;
	unsigned int forks0;

	worker_cfg(i, i, extra);
	if (!crash(i) || winfo(i).state != AWORKER_ST_RESTARTING)
		return 0;
	inf = winfo(i);
	forks0 = g_forks;
	fake_advance_ms(inf.restart_delay * 1000UL);
	inf = winfo(i);
	return g_forks == forks0 + 1 && inf.state == AWORKER_ST_STARTING ? inf.pid : 0;
}

struct start_result {
	bool started;
	bool ok[12];		/*!< the stages, see each test */
	bool w0_untouched;
	bool no_shutdown;
};

static void start_run(char **argv, void (*fn)(char **argv, struct start_result *res), struct start_result *res)
{
	int pp[2], status;
	pid_t bsc;

	OSMO_ASSERT(pipe(pp) == 0);
	bsc = fork();
	OSMO_ASSERT(bsc >= 0);
	if (bsc == 0) {
		close(pp[0]);
		memset(res, 0, sizeof(*res));
		fn(argv, res);
		res->no_shutdown = !osmo_select_shutdown_requested() && !aworker_pool_shutdown_was_fatal();
		fake_clock_off();
		aworker_pool_stop();
		_exit(write(pp[1], res, sizeof(*res)) == sizeof(*res) ? 0 : 1);
	}
	close(pp[1]);
	CHECK(read(pp[0], res, sizeof(*res)) == sizeof(*res));
	close(pp[0]);
	CHECK(collect(bsc, &status, 60000) == bsc);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(res->started);
	CHECK(res->no_shutdown);
}

/* P17: two workers restarted at different times, each never READY: each times out exactly 60 s after its own start,
 * only that worker is affected, and the loss goes through the restart policy (backoff), after which a worker that
 * reports its cs7 instances is READY and stays so. */
static void p17_fn(char **argv, struct start_result *res)
{
	struct aworker_pool_worker_info i0;
	pid_t p0, p1, p0b;

	res->started = backoff_pool_start(argv, NULL, 60);
	if (!res->started)
		return;
	p0 = restart_with(0, "no_cs7=1\n");	/* lost at 0 s, started at 5 s: times out at 65 s */
	fake_advance_to(20000);
	p1 = restart_with(1, "no_cs7=1\n");	/* lost at 20 s, started at 25 s: times out at 85 s */
	if (!p0 || !p1)
		return;

	fake_advance_to(64999);
	res->ok[0] = w_is(0, AWORKER_ST_STARTING, p0) && w_is(1, AWORKER_ST_STARTING, p1);
	fake_advance_to(65000);
	res->ok[1] = !w_is(0, AWORKER_ST_STARTING, 0) && w_is(1, AWORKER_ST_STARTING, p1);

	/* Its next process will report its cs7 instances. */
	worker_cfg(0, 0, NULL);
	if (!collected(0))
		return;
	i0 = winfo(0);
	/* one loss more on record, the backoff continued: handled once, by the restart policy */
	res->ok[2] = i0.state == AWORKER_ST_RESTARTING && i0.restarts_in_window == 2 && i0.restart_delay == 10
		     && i0.backoff_step == 2 && w_is(1, AWORKER_ST_STARTING, p1);
	res->ok[3] = restarts_after(0, 10);	/* restarted after 10 s, READY */
	p0b = winfo(0).pid;

	fake_advance_to(84999);
	res->ok[4] = w_is(1, AWORKER_ST_STARTING, p1) && w_is(0, AWORKER_ST_READY, p0b);
	fake_advance_to(85000);
	res->ok[5] = !w_is(1, AWORKER_ST_STARTING, 0) && collected(1) && winfo(1).restarts_in_window == 2
		     && w_is(0, AWORKER_ST_READY, p0b);

	/* Worker 0's last start was at about 75 s: it stays READY long past 135 s, its timeout never fires. */
	fake_advance_to(200000);
	i0 = winfo(0);
	res->ok[6] = w_is(0, AWORKER_ST_READY, p0b) && i0.starts == 3 && i0.restarts_in_window == 2;
}

static void test_p17_startup_timeout_per_start(char **argv)
{
	struct start_result res;

	printf("P17: each start has its own startup timeout ('startup-timeout 60')\n");
	start_run(argv, p17_fn, &res);
	CHECK(res.ok[0]);
	CHECK(res.ok[1]);
	printf("  worker 0 started at 5 s, worker 1 at 25 s, neither READY: at 64.999 s both still starting;\n");
	printf("  at 65 s worker 0 is timed out, worker 1 keeps its process\n");
	CHECK(res.ok[2]);
	printf("  the timeout is one more unplanned loss: second restart in the window, backoff 10 s\n");
	CHECK(res.ok[3]);
	printf("  worker 0 restarted after 10 s and became READY\n");
	CHECK(res.ok[4]);
	CHECK(res.ok[5]);
	printf("  worker 1 is timed out at 85 s, its own deadline, not before; worker 0 stays READY with its process\n");
	CHECK(res.ok[6]);
	printf("  the READY worker 0 is never timed out later (200 s: same process, no further loss)\n");
}

/* P18's worker writes its heartbeats to a file of its own, which no earlier process wrote to. */
static char p18_hb[PATH_MAX];

/* True once that worker has counted its first heartbeat: its 10 ms timer runs, so its SIGUSR2 handler -- installed
 * before that timer is set up (fake_worker_main()) -- is in place. Sending SIGUSR2 earlier would kill it (default
 * action). */
static bool p18_heartbeat(void *data)
{
	unsigned long v = 0;
	FILE *f = fopen(p18_hb, "r");

	if (f) {
		if (fscanf(f, "%lu", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v >= 1;
}

/* P18: a worker that becomes READY 1 ms before its deadline is not timed out, then or later. */
static void p18_fn(char **argv, struct start_result *res)
{
	char extra[PATH_MAX + 64];
	pid_t p1;

	res->started = backoff_pool_start(argv, NULL, 60);
	if (!res->started)
		return;
	snprintf(p18_hb, sizeof(p18_hb), "%s/p18_heartbeat1", g_dir);
	snprintf(extra, sizeof(extra), "cs7_on_usr2=1\nheartbeat=%s\n", p18_hb);
	p1 = restart_with(1, extra);	/* started at 5 s: deadline 65 s */
	if (!p1)
		return;
	fake_advance_to(64999);
	res->ok[0] = w_is(1, AWORKER_ST_STARTING, p1);
	/* Only once its SIGUSR2 handler is in place (real time; the fake clock stands still meanwhile). */
	res->ok[4] = run_until(p18_heartbeat, NULL, 10000);
	if (!res->ok[4])
		return;
	kill(p1, SIGUSR2);
	res->ok[1] = run_until(w1_ready, NULL, 5000) && winfo(1).pid == p1;	/* the fake clock stands still */
	fake_advance_to(65000);
	res->ok[2] = w_is(1, AWORKER_ST_READY, p1);
	/* 250 s: no further loss (the restart of 0 s is still within the window); 300 s: same process. */
	fake_advance_to(250000);
	res->ok[3] = w_is(1, AWORKER_ST_READY, p1) && winfo(1).starts == 2 && winfo(1).restarts_in_window == 1;
	fake_advance_to(300000);
	res->ok[3] = res->ok[3] && w_is(1, AWORKER_ST_READY, p1) && winfo(1).starts == 2;
	res->w0_untouched = winfo(0).state == AWORKER_ST_READY && winfo(0).starts == 1;
}

static void test_p18_ready_before_deadline(char **argv)
{
	struct start_result res;

	printf("P18: a worker that becomes READY just before its startup deadline is not timed out\n");
	start_run(argv, p18_fn, &res);
	/* The worker counted a heartbeat (its SIGUSR2 handler is in place) before SIGUSR2 was sent. */
	CHECK(res.ok[4]);
	CHECK(res.ok[0] && res.ok[1]);
	printf("  started at 5 s, READY at 64.999 s\n");
	CHECK(res.ok[2] && res.ok[3]);
	printf("  at 65 s and up to 300 s: READY with the same process, no loss\n");
	CHECK(res.w0_untouched);
}

/* P19: the startup timeout of a process never outlives it. Worker 1, never READY:
 *  A started at 5 s (deadline 65 s), lost at 30 s;  B started at 40 s: not timed out at 65 s;
 *  B lost at 99 s, 1 s before its deadline; restart after 20 s: at 100 s, while the worker waits, nothing
 *    happens (no second loss, the restart still at 119 s);
 *  C started at 119 s: timed out at 179 s, its own deadline.
 * Then osmo-bsc stops while D is starting: its pending startup timer must go with the pool. */
static void p19_fn(char **argv, struct start_result *res)
{
	struct aworker_pool_worker_info i1;
	pid_t a, b, c;
	unsigned long end;

	res->started = backoff_pool_start(argv, NULL, 60);
	if (!res->started)
		return;
	a = restart_with(1, "no_cs7=1\n");		/* lost at 0 s: A started at 5 s */
	fake_advance_to(30000);
	b = restart_with(1, "no_cs7=1\n");		/* A lost at 30 s, backoff 10 s: B started at 40 s */
	if (!a || !b || a == b)
		return;
	res->ok[0] = g_fake_ms == 40000;
	fake_advance_to(65000);
	res->ok[1] = w_is(1, AWORKER_ST_STARTING, b);

	fake_advance_to(99000);
	worker_cfg(1, 1, "no_cs7=1\n");
	if (!crash(1))					/* B lost at 99 s: backoff 20 s, restart at 119 s */
		return;
	fake_advance_to(100000);
	i1 = winfo(1);
	res->ok[2] = i1.state == AWORKER_ST_RESTARTING && i1.restarts_in_window == 3 && i1.restart_delay == 20
		     && i1.backoff_step == 3;
	fake_advance_to(118999);
	res->ok[2] = res->ok[2] && w_is(1, AWORKER_ST_RESTARTING, 0);
	fake_advance_to(119000);
	c = winfo(1).pid;
	res->ok[2] = res->ok[2] && w_is(1, AWORKER_ST_STARTING, 0) && c != b;

	fake_advance_to(178999);
	res->ok[3] = w_is(1, AWORKER_ST_STARTING, c);
	fake_advance_to(179000);
	res->ok[3] = res->ok[3] && !w_is(1, AWORKER_ST_STARTING, 0) && collected(1);
	i1 = winfo(1);
	res->ok[4] = i1.restarts_in_window == 4 && i1.restart_delay == 40;
	res->w0_untouched = winfo(0).state == AWORKER_ST_READY && winfo(0).starts == 1;

	/* D starts (still never READY); osmo-bsc stops while it is STARTING. D lingers 300 ms after GOODBYE, so its link
	 * does not close (which would cancel the timer) before aworker_pool_stop() frees it: only the stop itself can
	 * take that timer down. */
	worker_cfg(1, 1, "no_cs7=1\nlinger=300\n");
	fake_advance_ms(i1.restart_delay * 1000UL);
	res->ok[5] = winfo(1).state == AWORKER_ST_STARTING;
	/* The fake clock keeps running through the stop (both workers exit by themselves after GOODBYE, so its waits
	 * end), and then passes D's deadline: a startup timer left behind would now fire on the freed worker. */
	aworker_pool_stop();
	fake_advance_ms(120 * 1000);
	end = now_ms() + 200;
	while (now_ms() < end)
		osmo_select_main_ctx(1);
	fake_clock_off();
	res->ok[6] = true;
}

static void test_p19_old_timeout_not_replacement(char **argv)
{
	struct start_result res;

	printf("P19: a startup timeout never outlives its process\n");
	start_run(argv, p19_fn, &res);
	CHECK(res.ok[0] && res.ok[1]);
	printf("  process started at 5 s lost at 30 s; its replacement, started at 40 s, is not timed out at 65 s\n");
	CHECK(res.ok[2]);
	printf("  that one lost at 99 s: its deadline (100 s) passes while the worker waits, without effect;\n");
	printf("  the next process starts at 119 s as scheduled\n");
	CHECK(res.ok[3] && res.ok[4]);
	printf("  which is timed out at 179 s, its own deadline: fourth restart in the window, backoff 40 s\n");
	CHECK(res.w0_untouched);
	CHECK(res.ok[5] && res.ok[6]);
	printf("  stopping osmo-bsc while a process is starting leaves no startup timer behind\n");
}

/* P20: the built-in default startup-timeout is 60 s, per start; an explicit setting replaces it. Worker 1's process
 * started at 5 s never becomes READY: timed out at 65 s with the default, at 35 s with 'startup-timeout 30'. */
static unsigned int g_p20_timeout;

static void p20_fn(char **argv, struct start_result *res)
{
	pid_t p1;
	unsigned long deadline = 5000 + g_p20_timeout * 1000UL;

	res->started = backoff_pool_start(argv, NULL, g_p20_timeout);
	if (!res->started)
		return;
	p1 = restart_with(1, "no_cs7=1\n");		/* started at 5 s */
	if (!p1)
		return;
	fake_advance_to(deadline - 1);
	res->ok[0] = w_is(1, AWORKER_ST_STARTING, p1);
	fake_advance_to(deadline);
	res->ok[1] = !w_is(1, AWORKER_ST_STARTING, 0) && collected(1) && winfo(1).restarts_in_window == 2;
	res->w0_untouched = winfo(0).state == AWORKER_ST_READY && winfo(0).starts == 1;
}

static void test_p20_startup_timeout_default(char **argv)
{
	struct start_result res;

	printf("P20: startup-timeout is 60 s by default; a configured value replaces it\n");
	CHECK(g_startup_timeout_default == 60);
	g_p20_timeout = g_startup_timeout_default;
	start_run(argv, p20_fn, &res);
	CHECK(res.ok[0] && res.ok[1] && res.w0_untouched);
	printf("  default: a process started at 5 s is still starting at 64.999 s, timed out at 65 s\n");
	g_p20_timeout = 30;
	start_run(argv, p20_fn, &res);
	CHECK(res.ok[0] && res.ok[1] && res.w0_untouched);
	printf("  'startup-timeout 30': still starting at 34.999 s, timed out at 35 s\n");
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
	g_startup_timeout_default = aworker_pool_cfg()->startup_timeout;
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
	test_p8_failed_worker(argv);
	test_p9_ownership_failed(argv);
	test_p10_backoff(argv);
	test_p11_stable_time(argv);
	test_p12_startup_not_stable(argv);
	test_p13_restart_limit(argv);
	test_p14_restart_window(argv);
	test_p15_stop_not_counted(argv);
	test_p16_stable_keeps_record(argv);
	test_p17_startup_timeout_per_start(argv);
	test_p18_ready_before_deadline(argv);
	test_p19_old_timeout_not_replacement(argv);
	test_p20_startup_timeout_default(argv);

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

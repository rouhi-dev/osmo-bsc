/* The Supervisor's sessions (S2-5 stage 4): socket security, admission, attach/detach, ECHO, fencing, operator */
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

/* X: the real bsc-aworker (argv[1]) as Supervisor, with two inline workers whose asps point at a free local port
 * nobody listens on. The BSC is scripted: in this process for most cases, and as a process of its own -- this very
 * program re-executed with --bsc-client -- where it must be stopped, killed or fenced. An operator is scripted in this
 * process. Nothing here needs privileges: a foreign peer uid is simulated with the Supervisor's peer-uid test hook. */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

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

#include <osmocom/bsc/aworker_canon.h>
#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

static void *ctx;
static unsigned int failures;
static const char *g_bin;
static char g_self[512];
static char g_dir[256];
static char g_sock[300];
static char g_cfg[300];
static char *g_doc;
static size_t g_doc_len;

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			fflush(stdout); \
			failures++; \
		} \
	} while (0)

static const struct log_info test_log_info = {
	.cat = osmo_bsc_categories,
	.num_cat = Debug_LastEntry,
};

static struct vty_app_info vty_info = {
	.name = "aworker_session_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

static unsigned int ms_since(const struct timespec *t0)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

#define WAIT_FOR(cond, ms) do { \
		struct timespec _t0; \
		clock_gettime(CLOCK_MONOTONIC, &_t0); \
		while (!(cond) && ms_since(&_t0) < (ms)) { \
			osmo_select_main(1); \
			usleep(1000); \
		} \
	} while (0)

static void pump_ms(unsigned int ms)
{
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (ms_since(&t0) < ms) {
		osmo_select_main(1);
		usleep(1000);
	}
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

static unsigned int fds_of(pid_t pid)
{
	char p[64];
	struct dirent *de;
	unsigned int n = 0;
	DIR *d;

	snprintf(p, sizeof(p), "/proc/%ld/fd", (long)pid);
	if (!(d = opendir(p)))
		return 0;
	while ((de = readdir(d)))
		n += de->d_name[0] != '.';
	closedir(d);
	return n;
}

/* The processes (of this user) holding a descriptor of socket inode ino; up to max pids into out. */
static unsigned int holders(unsigned long ino, pid_t *out, unsigned int max)
{
	char want[48], p[300], l[64];
	struct dirent *pe, *fe;
	unsigned int n = 0;
	DIR *pd, *fd;

	snprintf(want, sizeof(want), "socket:[%lu]", ino);
	if (!(pd = opendir("/proc")))
		return 0;
	while ((pe = readdir(pd))) {
		bool has = false;
		if (pe->d_name[0] < '1' || pe->d_name[0] > '9')
			continue;
		snprintf(p, sizeof(p), "/proc/%s/fd", pe->d_name);
		if (!(fd = opendir(p)))
			continue;
		while (!has && (fe = readdir(fd))) {
			char lp[600];
			ssize_t r;
			snprintf(lp, sizeof(lp), "%s/%s", p, fe->d_name);
			r = readlink(lp, l, sizeof(l) - 1);
			if (r > 0) {
				l[r] = '\0';
				has = !strcmp(l, want);
			}
		}
		closedir(fd);
		if (has && n < max)
			out[n++] = atoi(pe->d_name);
		else if (has)
			n++;
	}
	closedir(pd);
	return n;
}

/* The peer inode of a connected AF_UNIX socket inode, from ss(8). 0: not found. */
static unsigned long peer_inode(unsigned long ino)
{
	char line[1024];
	unsigned long a, b, found = 0;
	FILE *f = popen("ss -xan 2>/dev/null", "r");

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f)) {
		char *star1 = strchr(line, '*');
		char *star2 = star1 ? strchr(star1 + 1, '*') : NULL;
		if (!star1 || !star2)
			continue;
		if (sscanf(star1 + 1, "%lu", &a) == 1 && sscanf(star2 + 1, "%lu", &b) == 1 && b == ino)
			found = a;
	}
	pclose(f);
	return found;
}

static bool file_has(const char *path, const char *text)
{
	static char buf[1 << 20];
	int fd = open(path, O_RDONLY);
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

static unsigned int file_count(const char *path, const char *text)
{
	static char buf[1 << 20];
	int fd = open(path, O_RDONLY);
	unsigned int c = 0;
	ssize_t n;
	char *p;

	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 0)
		return 0;
	buf[n] = '\0';
	for (p = buf; (p = strstr(p, text)); p++)
		c++;
	return c;
}

/* A sealed memfd with doc. */
static int doc_memfd(const char *doc, size_t len, bool seal)
{
	int fd = memfd_create("canon", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	OSMO_ASSERT(fd >= 0 && write(fd, doc, len) == (ssize_t)len);
	if (seal)
		OSMO_ASSERT(fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) == 0);
	return fd;
}

/***********************************************************************
 * The configuration and the Supervisor
 ***********************************************************************/

static void write_cfg(const char *path, const char *sock, const char *extra)
{
	unsigned int port = free_port();
	FILE *f = fopen(path, "w");

	OSMO_ASSERT(f);
	fprintf(f,
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
		"cs7 instance 1\n"
		" point-code 0.23.4\n"
		" asp asp-w1 %u 0 m3ua tcp\n"
		"  remote-ip 127.0.0.1\n"
		"  local-ip 127.0.0.1\n"
		"  role asp\n"
		"  transport-role client\n"
		"a-worker-pool\n"
		" workers 2\n"
		" worker 0 cs7-instance 0\n"
		" worker 1 cs7-instance 1\n"
		" restart-delay 1\n"
		" supervisor-socket %s\n"
		" session-echo-interval 1\n"
		" session-echo-misses 2\n"
		"%s", port, port, sock, extra ? extra : "");
	OSMO_ASSERT(fclose(f) == 0);
}

struct sup {
	pid_t pid;
	char log[300];
};

/* Start bsc-aworker -c cfg with test hooks (NULL-terminated list of NAME[=VALUE]). */
static void sup_start(struct sup *s, const char *cfg, const char *tag, const char *const *hooks)
{
	const char *argv[24];
	unsigned int n = 0;
	int fd;

	snprintf(s->log, sizeof(s->log), "%s/supervisor.%s.log", g_dir, tag);
	s->pid = fork();
	OSMO_ASSERT(s->pid >= 0);
	if (s->pid == 0) {
		/* never outlive the test, whatever happens to it */
		prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
		fd = open(s->log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || dup2(fd, 2) < 0)
			_exit(99);
		fd = open("/dev/null", O_RDWR);
		dup2(fd, 0);
		dup2(fd, 1);
		close_range(3, ~0U, 0);
		argv[n++] = g_bin;
		argv[n++] = "-c";
		argv[n++] = cfg;
		while (hooks && *hooks) {
			argv[n++] = "--aworker-test-hook";
			argv[n++] = *hooks++;
		}
		argv[n] = NULL;
		execv(g_bin, (char **)argv);
		_exit(98);
	}
}

static int sup_wait(struct sup *s, unsigned int ms)
{
	int status = 0;
	pid_t r = 0;

	WAIT_FOR((r = waitpid(s->pid, &status, WNOHANG)) == s->pid, ms);
	if (r != s->pid) {
		kill(s->pid, SIGKILL);
		waitpid(s->pid, &status, 0);
		return -1;
	}
	s->pid = 0;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void sup_stop(struct sup *s)
{
	if (s->pid > 0) {
		kill(s->pid, SIGTERM);
		CHECK(sup_wait(s, 15000) == 0);
	}
}

/***********************************************************************
 * A client (BSC or operator) in this process
 ***********************************************************************/

struct cli {
	struct aworker_ipc_link *link;
	unsigned int ready, closed;
	struct aworker_ipc_link_close_info ci;
	bool accepted, rejected, got_close;
	uint32_t session_id;
	uint64_t nonce;
	uint8_t reject_reason, close_reason;
	char text[AWORKER_IPC_MAX_TEXT + 1];
	unsigned int n_up, n_down, n_state, n_result;
	int up_fd[2];
	uint32_t up_inc[2];
	uint8_t down_reason[2];
	unsigned int down_count[2];
	uint8_t att[2];
	uint8_t op;
	int32_t rc;
};

static void c_ready(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	struct cli *c = aworker_ipc_link_get_priv(link);
	c->ready++;
}

static void c_rx(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	struct cli *c = aworker_ipc_link_get_priv(link);
	unsigned int i;

	switch (m->type) {
	case AWORKER_IPC_MT_SESSION_ACCEPT:
		c->accepted = true;
		c->session_id = m->u.session_accept.session_id;
		c->nonce = m->u.session_accept.boot_nonce;
		break;
	case AWORKER_IPC_MT_SESSION_REJECT:
		c->rejected = true;
		c->reject_reason = m->u.session_reject.reason;
		snprintf(c->text, sizeof(c->text), "%.*s", m->text.len, (const char *)m->text.buf);
		break;
	case AWORKER_IPC_MT_SESSION_CLOSE:
		c->got_close = true;
		c->close_reason = m->u.session_close.reason;
		break;
	case AWORKER_IPC_MT_WORKER_UP:
		i = m->u.worker_up.worker_idx;
		c->n_up++;
		if (i < 2) {
			if (c->up_fd[i] >= 0)
				close(c->up_fd[i]);
			c->up_fd[i] = aworker_ipc_link_take_fd(link);
			c->up_inc[i] = m->u.worker_up.incarnation;
		}
		break;
	case AWORKER_IPC_MT_WORKER_DOWN:
		i = m->u.worker_down.worker_idx;
		c->n_down++;
		if (i < 2) {
			c->down_reason[i] = m->u.worker_down.reason;
			c->down_count[i]++;
		}
		break;
	case AWORKER_IPC_MT_WORKER_STATE:
		i = m->u.worker_state.worker_idx;
		c->n_state++;
		if (i < 2)
			c->att[i] = m->u.worker_state.attach;
		break;
	case AWORKER_IPC_MT_OP_RESULT:
		c->n_result++;
		c->op = m->u.op_result.op;
		c->rc = m->u.op_result.rc;
		snprintf(c->text, sizeof(c->text), "%.*s", m->text.len, (const char *)m->text.buf);
		break;
	default:
		break;
	}
}

static void c_closed(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	struct cli *c = aworker_ipc_link_get_priv(link);
	c->closed++;
	c->ci = *info;
}

static const struct aworker_ipc_link_ops c_ops = { .ready_cb = c_ready, .rx_cb = c_rx, .closed_cb = c_closed };

static int sock_connect(const char *path)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

	OSMO_STRLCPY_ARRAY(a.sun_path, path);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static bool cli_open(struct cli *c, const char *path)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_CLIENT, .worker_idx = 0, .pid = (uint32_t)getpid(), .ident = "test-client",
		.log_cat = DLGLOBAL, .classes = AWORKER_IPC_CLASS_SESSION, .fd_passing = true,
	};
	int fd;

	memset(c, 0, sizeof(*c));
	c->up_fd[0] = c->up_fd[1] = -1;
	fd = sock_connect(path);
	if (fd < 0)
		return false;
	OSMO_ASSERT(aworker_ipc_link_alloc(&c->link, ctx, fd, &cfg, &c_ops, c) == 0);
	return true;
}

static void cli_free(struct cli *c)
{
	if (c->link)
		aworker_ipc_link_free(c->link);
	c->link = NULL;
	if (c->up_fd[0] >= 0)
		close(c->up_fd[0]);
	if (c->up_fd[1] >= 0)
		close(c->up_fd[1]);
	c->up_fd[0] = c->up_fd[1] = -1;
}

/* SESSION_OPEN with doc (a memfd, sealed or not; doc_len as said) and version; wait for ACCEPT/REJECT. */
static void session_open(struct cli *c, const char *doc, size_t len, uint32_t said_len, uint8_t version, bool seal)
{
	struct msgb *msg;
	int fd;

	/* nothing may be sent before the handshake completed */
	WAIT_FOR(c->ready || c->closed, 5000);
	fd = doc_memfd(doc, len, seal);
	OSMO_ASSERT(aworker_ipc_enc_session_open(ctx, &msg, version, said_len) == 0);
	CHECK(aworker_ipc_link_send_fd(c->link, msg, fd, NULL) == 0);
	WAIT_FOR(c->accepted || c->rejected || c->closed, 5000);
}

static int op(const char *path, uint8_t o, uint16_t arg, char *text, size_t tlen)
{
	struct cli c;
	struct msgb *msg;
	int rc;

	if (!cli_open(&c, path))
		return -ECONNREFUSED;
	WAIT_FOR(c.ready || c.closed, 5000);
	if (!c.ready) {
		if (text)
			snprintf(text, tlen, "(refused)");
		cli_free(&c);
		return -ECONNRESET;
	}
	OSMO_ASSERT(aworker_ipc_enc_op_request(ctx, &msg, o, arg) == 0);
	CHECK(aworker_ipc_link_send(c.link, msg, NULL) == 0);
	WAIT_FOR(c.n_result || c.closed, 8000);
	rc = c.n_result ? c.rc : -EPIPE;
	if (text)
		snprintf(text, tlen, "%s", c.n_result ? c.text : "(no result)");
	cli_free(&c);
	return rc;
}

/* STATUS without what differs from run to run: ", pid N" and the BSC's ", BSC pid N uid N". */
static void normalize(char *t)
{
	char *p;

	while ((p = strstr(t, ", BSC pid "))) {
		char *e = p + 10;
		e += strspn(e, "0123456789");
		if (!strncmp(e, " uid ", 5))
			e += 5 + strspn(e + 5, "0123456789");
		memmove(p, e, strlen(e) + 1);
	}
	while ((p = strstr(t, ", pid "))) {
		char *e = p + 6 + strspn(p + 6, "0123456789");
		memmove(p, e, strlen(e) + 1);
	}
	/* the number of unanswered ECHOs depends on timing */
	if ((p = strstr(t, "ECHO misses "))) {
		char *e = p + 12 + strspn(p + 12, "0123456789");
		memmove(p + 12, "N", 1);
		memmove(p + 13, e, strlen(e) + 1);
	}
}

static bool status_has(const char *text)
{
	char t[AWORKER_IPC_MAX_TEXT + 1];
	if (op(g_sock, AWORKER_IPC_OP_STATUS, 0, t, sizeof(t)) != 0)
		return false;
	normalize(t);
	return strstr(t, text) != NULL;
}

/* Wait until STATUS contains text. */
static bool wait_status(const char *text, unsigned int ms)
{
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (ms_since(&t0) < ms) {
		if (status_has(text))
			return true;
		pump_ms(50);
	}
	return false;
}

static void print_status(const char *what)
{
	char t[AWORKER_IPC_MAX_TEXT + 1], *l, *save;
	unsigned int n;

	op(g_sock, AWORKER_IPC_OP_STATUS, 0, t, sizeof(t));
	normalize(t);
	printf("  %s:\n", what);
	for (l = strtok_r(t, "\n", &save), n = 0; l; l = strtok_r(NULL, "\n", &save), n++)
		printf("    %s\n", l);
}

/* Data links to the workers over the WORKER_UP descriptors: both handshakes complete. */
struct dl {
	struct aworker_ipc_link *link;
	unsigned int ready, closed;
	struct aworker_ipc_link_close_info ci;
};

static void d_ready(struct aworker_ipc_link *l, const struct aworker_ipc_hello *p)
{
	((struct dl *)aworker_ipc_link_get_priv(l))->ready++;
}

static void d_rx(struct aworker_ipc_link *l, const struct aworker_ipc_msg *m)
{
}

static void d_closed(struct aworker_ipc_link *l, const struct aworker_ipc_link_close_info *i)
{
	struct dl *d = aworker_ipc_link_get_priv(l);
	d->closed++;
	d->ci = *i;
}

static const struct aworker_ipc_link_ops d_ops = { .ready_cb = d_ready, .rx_cb = d_rx, .closed_cb = d_closed };

static void data_links(struct cli *c, struct dl d[2])
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		struct aworker_ipc_link_cfg cfg = {
			.role = AWORKER_IPC_ROLE_MAIN, .worker_idx = i, .pid = (uint32_t)getpid(), .ident = "test-bsc",
			.log_cat = DLGLOBAL, .classes = AWORKER_IPC_CLASS_DATA,
			.id = { .boot_nonce = c->nonce, .incarnation = c->up_inc[i], .session_id = c->session_id },
		};
		memset(&d[i], 0, sizeof(d[i]));
		OSMO_ASSERT(c->up_fd[i] >= 0);
		OSMO_ASSERT(aworker_ipc_link_alloc(&d[i].link, ctx, c->up_fd[i], &cfg, &d_ops, &d[i]) == 0);
		c->up_fd[i] = -1;
	}
	WAIT_FOR(d[0].ready && d[1].ready, 5000);
}

static void data_free(struct dl d[2])
{
	unsigned int i;
	for (i = 0; i < 2; i++) {
		if (d[i].link)
			aworker_ipc_link_free(d[i].link);
		d[i].link = NULL;
	}
}

/* A full session in this process: open, both WORKER_UPs and both ATTACHED. */
static bool full_session(struct cli *c, struct dl d[2])
{
	if (!cli_open(c, g_sock))
		return false;
	session_open(c, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
	WAIT_FOR(c->n_up == 2 || c->rejected || c->closed, 5000);
	if (!c->accepted || c->n_up != 2)
		return false;
	data_links(c, d);
	WAIT_FOR(c->att[0] == AWORKER_IPC_ATT_ATTACHED && c->att[1] == AWORKER_IPC_ATT_ATTACHED, 5000);
	return d[0].ready && d[1].ready && c->att[0] == AWORKER_IPC_ATT_ATTACHED
	       && c->att[1] == AWORKER_IPC_ATT_ATTACHED;
}

static void session_end(struct cli *c, struct dl d[2])
{
	struct msgb *msg;

	OSMO_ASSERT(aworker_ipc_enc_session_close(ctx, &msg, AWORKER_IPC_CC_NORMAL) == 0);
	aworker_ipc_link_send(c->link, msg, NULL);
	/* The workers close their data links (DETACH from the Supervisor), while we still hold ours. */
	WAIT_FOR(d[0].closed && d[1].closed, 5000);
	CHECK(d[0].closed && d[1].closed);
	data_free(d);
	cli_free(c);
}

/***********************************************************************
 * The BSC as a process of its own (--bsc-client)
 ***********************************************************************/

/* Child: open a session on argv sock with the document file docfile, make both data links, report on fd report
 * "ACCEPT <session> <ino0> <ino1>\n" (the BSC ends' socket inodes), then serve forever (ECHO is answered by the
 * link). On a refusal: "REJECT <reason>\n" and exit. */
static int bsc_client_main(const char *sock, const char *docfile, int report)
{
	struct cli c;
	struct dl d[2];
	struct stat st0, st1;
	char line[128];
	char *doc;
	int fd;
	struct stat st;

	fd = open(docfile, O_RDONLY);
	OSMO_ASSERT(fd >= 0 && fstat(fd, &st) == 0);
	doc = talloc_size(ctx, st.st_size);
	OSMO_ASSERT(read(fd, doc, st.st_size) == st.st_size);
	close(fd);
	if (!cli_open(&c, sock))
		return 2;
	session_open(&c, doc, st.st_size, st.st_size, AWORKER_IPC_SESSION_VERSION, true);
	if (!c.accepted) {
		snprintf(line, sizeof(line), "REJECT %u\n", c.reject_reason);
		OSMO_ASSERT(write(report, line, strlen(line)) > 0);
		return 1;
	}
	WAIT_FOR(c.n_up == 2, 5000);
	OSMO_ASSERT(c.n_up == 2 && fstat(c.up_fd[0], &st0) == 0 && fstat(c.up_fd[1], &st1) == 0);
	data_links(&c, d);
	snprintf(line, sizeof(line), "ACCEPT %u %lu %lu\n", c.session_id, (unsigned long)st0.st_ino,
		 (unsigned long)st1.st_ino);
	OSMO_ASSERT(write(report, line, strlen(line)) > 0);
	while (1)
		osmo_select_main(0);
	return 0;
}

struct bsc {
	pid_t pid;
	uint32_t session;
	unsigned long ino[2];
	bool accepted;
};

static void bsc_spawn(struct bsc *b)
{
	char docfile[300], rfd[16], line[128] = "";
	int p[2], fd;
	ssize_t n = 0, r;

	snprintf(docfile, sizeof(docfile), "%s/doc.txt", g_dir);
	fd = open(docfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	OSMO_ASSERT(fd >= 0 && write(fd, g_doc, g_doc_len) == (ssize_t)g_doc_len);
	close(fd);
	OSMO_ASSERT(pipe(p) == 0);
	memset(b, 0, sizeof(*b));
	b->pid = fork();
	OSMO_ASSERT(b->pid >= 0);
	if (b->pid == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
		close(p[0]);
		fd = open("/dev/null", O_RDWR);
		dup2(fd, 2);
		dup2(p[1], 9);
		close_range(10, ~0U, 0);
		for (fd = 3; fd < 9; fd++)
			close(fd);
		snprintf(rfd, sizeof(rfd), "9");
		execl(g_self, g_self, "--bsc-client", g_sock, docfile, rfd, (char *)NULL);
		_exit(98);
	}
	close(p[1]);
	while (n < (ssize_t)sizeof(line) - 1 && (r = read(p[0], line + n, sizeof(line) - 1 - n)) > 0) {
		n += r;
		if (memchr(line, '\n', n))
			break;
	}
	close(p[0]);
	line[n > 0 ? n : 0] = '\0';
	b->accepted = sscanf(line, "ACCEPT %u %lu %lu", &b->session, &b->ino[0], &b->ino[1]) == 3;
}

static void bsc_kill(struct bsc *b)
{
	if (b->pid > 0) {
		kill(b->pid, SIGKILL);
		waitpid(b->pid, NULL, 0);
	}
	b->pid = 0;
}

/***********************************************************************
 * The cases
 ***********************************************************************/

static void x_socket(void)
{
	char d2[400], cfg2[400], sock2[400], t[AWORKER_IPC_MAX_TEXT + 1];
	struct sup s1, s2;
	struct stat st;
	int fd;
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *peer_hook[2];
	char hookbuf[48];
	struct cli c;

	printf("X1: the socket (N5)\n");
	sup_start(&s1, g_cfg, "x1", NULL);
	WAIT_FOR(stat(g_sock, &st) == 0, 10000);
	CHECK(stat(g_sock, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
	snprintf(d2, sizeof(d2), "%s.lock", g_sock);
	CHECK(stat(d2, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
	printf("  socket mode 0600, lock file 0600\n");
	CHECK(wait_status("worker 1: READY", 15000));

	/* a second Supervisor on the same socket: refused before it starts any worker */
	sup_start(&s2, g_cfg, "x1-second", NULL);
	CHECK(sup_wait(&s2, 10000) == 1);
	CHECK(file_has(s2.log, "is held by another process: another Supervisor uses this socket"));
	CHECK(!file_has(s2.log, "started '"));
	printf("  a second Supervisor on the same socket exits 1 before starting workers\n");
	CHECK(status_has("worker 0: READY"));
	sup_stop(&s1);
	CHECK(stat(g_sock, &st) < 0 && errno == ENOENT);
	snprintf(d2, sizeof(d2), "%s.lock", g_sock);
	CHECK(stat(d2, &st) == 0);
	printf("  stopped: the socket is removed, the lock file stays\n");

	/* a leftover socket is removed (we hold the lock); anything else at the path is refused */
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	OSMO_STRLCPY_ARRAY(a.sun_path, g_sock);
	OSMO_ASSERT(fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
	close(fd);
	sup_start(&s1, g_cfg, "x1-stale", NULL);
	CHECK(wait_status("session: NONE", 10000));
	CHECK(file_has(s1.log, "removed the leftover socket"));
	sup_stop(&s1);
	printf("  a leftover socket is removed once the lock is held\n");
	fd = open(g_sock, O_WRONLY | O_CREAT, 0600);
	close(fd);
	sup_start(&s1, g_cfg, "x1-file", NULL);
	CHECK(sup_wait(&s1, 10000) == 1);
	CHECK(file_has(s1.log, "exists and is not a socket; not removing it"));
	unlink(g_sock);
	printf("  a regular file at the path: refused, left alone\n");

	/* the directory must be ours and writable by nobody else */
	snprintf(d2, sizeof(d2), "%s/open", g_dir);
	OSMO_ASSERT(mkdir(d2, 0700) == 0 && chmod(d2, 0770) == 0);
	snprintf(sock2, sizeof(sock2), "%s/s.sock", d2);
	snprintf(cfg2, sizeof(cfg2), "%s/open.cfg", g_dir);
	write_cfg(cfg2, sock2, NULL);
	sup_start(&s2, cfg2, "x1-dir", NULL);
	CHECK(sup_wait(&s2, 10000) == 1);
	CHECK(file_has(s2.log, "is writable by others (mode 770)"));
	printf("  a directory writable by its group: refused\n");
	snprintf(sock2, sizeof(sock2), "%s/missing/s.sock", g_dir);
	write_cfg(cfg2, sock2, NULL);
	sup_start(&s2, cfg2, "x1-nodir", NULL);
	CHECK(sup_wait(&s2, 10000) == 1);
	CHECK(file_has(s2.log, "(it is not created)"));
	printf("  a missing directory: refused, not created\n");

	printf("X2: a peer of another uid is refused before anything is read\n");
	snprintf(hookbuf, sizeof(hookbuf), "peer-uid=%u", (unsigned int)geteuid() + 1);
	peer_hook[0] = hookbuf;
	peer_hook[1] = NULL;
	sup_start(&s1, g_cfg, "x2", peer_hook);
	WAIT_FOR(stat(g_sock, &st) == 0, 10000);
	pump_ms(300);
	CHECK(cli_open(&c, g_sock));
	WAIT_FOR(c.closed, 5000);
	/* (how the close shows here -- EOF, or an error on reading or writing -- depends on timing) */
	printf("  our connection: %s, handshake %s\n", c.closed ? "closed by the Supervisor" : "open",
	       c.ready ? "done" : "never done");
	CHECK(c.closed && !c.ready);
	cli_free(&c);
	CHECK(op(g_sock, AWORKER_IPC_OP_STATUS, 0, t, sizeof(t)) < 0);
	CHECK(file_count(s1.log, "refused: uid") == 2);
	printf("  an operator of that uid is refused the same way\n");
	sup_stop(&s1);
}

static void x_admission(struct sup *s)
{
	struct cli c, c2;
	struct dl d[2];
	char *bad;
	unsigned int i, base;

	printf("X3: SESSION_OPEN admitted: ACCEPT, WORKER_UP per worker, data links, ATTACHED\n");
	CHECK(full_session(&c, d));
	printf("  session %u, boot nonce %s, WORKER_UP %u, incarnations %u/%u, attached %s/%s\n", c.session_id,
	       c.nonce ? "set" : "0", c.n_up, c.up_inc[0], c.up_inc[1], aworker_ipc_attach_state_name(c.att[0]),
	       aworker_ipc_attach_state_name(c.att[1]));
	{
		/* an SCCP user over worker 1's data link */
		struct msgb *msg;
		OSMO_ASSERT(aworker_ipc_enc_user_bind(ctx, &msg, 1, 1, 254, "msc-1") == 0);
		CHECK(aworker_ipc_link_send(d[1].link, msg, NULL) == 0);
		pump_ms(200);
		CHECK(!d[1].closed);
	}
	print_status("STATUS");

	printf("X4: a second BSC is busy; other refusals\n");
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
	printf("  second BSC: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BUSY);
	cli_free(&c2);
	session_end(&c, d);
	CHECK(wait_status("session: CLOSED", 5000));

	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len, 2, true);
	printf("  version 2: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_VERSION);
	cli_free(&c2);
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, false);
	printf("  document not sealed: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BAD_DOCUMENT);
	cli_free(&c2);
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len + 1, AWORKER_IPC_SESSION_VERSION, true);
	/* (its detail names the octet counts, which depend on the path of the test directory) */
	printf("  document length: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason),
	       strstr(c2.text, "SESSION_OPEN says") ? "the length differs" : c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BAD_DOCUMENT && strstr(c2.text, "SESSION_OPEN says"));
	cli_free(&c2);
	bad = talloc_strdup(ctx, g_doc);
	OSMO_ASSERT(strstr(bad, "P restart-delay 1\n"));
	strstr(bad, "P restart-delay 1\n")[16] = '2';
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, bad, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
	printf("  another configuration: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_CONFIG_MISMATCH && strstr(c2.text, "line 7, part P"));
	cli_free(&c2);
	talloc_free(bad);

	printf("X5: the operator cannot open a session on its connection; unknown operations close it\n");
	{
		struct msgb *msg;
		int fd;
		CHECK(cli_open(&c2, g_sock));
		WAIT_FOR(c2.ready, 5000);
		OSMO_ASSERT(aworker_ipc_enc_op_request(ctx, &msg, AWORKER_IPC_OP_STATUS, 0) == 0);
		CHECK(aworker_ipc_link_send(c2.link, msg, NULL) == 0);
		WAIT_FOR(c2.n_result, 5000);
		fd = doc_memfd(g_doc, g_doc_len, true);
		OSMO_ASSERT(aworker_ipc_enc_session_open(ctx, &msg, AWORKER_IPC_SESSION_VERSION, g_doc_len) == 0);
		CHECK(aworker_ipc_link_send_fd(c2.link, msg, fd, NULL) == 0);
		WAIT_FOR(c2.closed, 5000);
		printf("  SESSION_OPEN after OP_REQUEST: connection %s\n", c2.closed ? "closed" : "OPEN");
		CHECK(c2.closed && !c2.accepted);
		cli_free(&c2);
		/* an operation code that does not exist (raw record): the codec refuses it, the link closes */
		CHECK(cli_open(&c2, g_sock));
		OSMO_ASSERT(aworker_ipc_enc_op_request(ctx, &msg, AWORKER_IPC_OP_STATUS, 0) == 0);
		msgb_data(msg)[AWORKER_IPC_HDR_LEN] = 9;
		{
			int sfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
			struct sockaddr_un a = { .sun_family = AF_UNIX };
			char b[256];
			OSMO_STRLCPY_ARRAY(a.sun_path, g_sock);
			OSMO_ASSERT(connect(sfd, (struct sockaddr *)&a, sizeof(a)) == 0);
			struct msgb *h;
			OSMO_ASSERT(aworker_ipc_enc_hello(ctx, &h, AWORKER_IPC_ROLE_CLIENT, 0, getpid(), "raw") == 0);
			aworker_ipc_frame_set_seq(msgb_data(h), 1);
			CHECK(send(sfd, msgb_data(h), msgb_length(h), 0) > 0);
			aworker_ipc_frame_set_seq(msgb_data(msg), 2);
			CHECK(send(sfd, msgb_data(msg), msgb_length(msg), 0) > 0);
			msgb_free(h);
			/* the Supervisor's HELLO, then EOF */
			while (recv(sfd, b, sizeof(b), 0) > 0)
				;
			close(sfd);
			printf("  operation 9: connection closed by the Supervisor\n");
		}
		msgb_free(msg);
		cli_free(&c2);
		CHECK(file_has(s->log, "received malformed record"));
	}

	printf("X6: session ids are never reused; 100 attach/detach cycles leave the Supervisor's descriptors as they"
	       " were\n");
	CHECK(full_session(&c, d));
	printf("  next session: %u\n", c.session_id);
	CHECK(c.session_id == 2);
	session_end(&c, d);
	CHECK(wait_status("session: CLOSED", 5000));
	pump_ms(200);
	base = fds_of(s->pid);
	for (i = 0; i < 100; i++) {
		if (!full_session(&c, d)) {
			CHECK(0);
			break;
		}
		session_end(&c, d);
		WAIT_FOR(status_has("session: CLOSED"), 5000);
	}
	pump_ms(200);
	printf("  100 cycles: Supervisor descriptors %s\n", fds_of(s->pid) == base ? "unchanged" : "CHANGED");
	CHECK(fds_of(s->pid) == base);
	CHECK(status_has("CLOSING entered: 102"));
}

static void x_unresponsive(void)
{
	struct bsc b;
	struct cli c2;

	printf("X7: an UNRESPONSIVE BSC: nothing happens to the workers, and no second BSC is admitted\n");
	bsc_spawn(&b);
	CHECK(b.accepted);
	CHECK(wait_status("worker 1: READY, incarnation 1, attached", 5000));
	kill(b.pid, SIGSTOP);
	CHECK(wait_status("session: UNRESPONSIVE", 6000));
	pump_ms(3000);	/* well past session-echo-misses */
	CHECK(status_has("session: UNRESPONSIVE"));
	CHECK(status_has("worker 0: READY, incarnation 1, attached"));
	CHECK(status_has("worker 1: READY, incarnation 1, attached"));
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
	printf("  second BSC while UNRESPONSIVE: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BUSY);
	cli_free(&c2);
	print_status("STATUS while the BSC is stopped");
	kill(b.pid, SIGCONT);
	CHECK(wait_status("session: ACTIVE", 5000));
	printf("  answering again: ACTIVE\n");
	bsc_kill(&b);
	CHECK(wait_status("session: CLOSED", 5000));
}

static void x_fence(void)
{
	char t[AWORKER_IPC_MAX_TEXT + 1];
	struct bsc b;
	int rc;

	printf("X8: fencing: SIGKILL through the pidfd, exit confirmed, CLOSING once, then a new BSC is admitted\n");
	bsc_spawn(&b);
	CHECK(b.accepted);
	CHECK(wait_status("fencing available", 3000));
	rc = op(g_sock, AWORKER_IPC_OP_FENCE_SESSION, 0, t, sizeof(t));
	/* Either the pidfd reports the exit first, or the dead BSC's EOF on the session socket arrives first: both
	 * close the session (SUP 9.1: "FENCE, then EOF before the pidfd reports: CLOSING"). */
	printf("  FENCE_SESSION: rc %d, %s\n", rc, strstr(t, "exit confirmed") || strstr(t, "connection to the BSC: EOF")
	       ? "fenced (exit confirmed, or its EOF first)" : t);
	CHECK(rc == 0 && (strstr(t, "exit confirmed") || strstr(t, "connection to the BSC: EOF")));
	CHECK(waitpid(b.pid, &rc, 0) == b.pid && WIFSIGNALED(rc) && WTERMSIG(rc) == SIGKILL);
	b.pid = 0;
	CHECK(wait_status("session: CLOSED", 5000));
	rc = op(g_sock, AWORKER_IPC_OP_FENCE_SESSION, 0, t, sizeof(t));
	printf("  FENCE_SESSION without a session: rc %d (%s)\n", rc, t);
	CHECK(rc == -ENOENT);
	bsc_spawn(&b);
	CHECK(b.accepted);
	printf("  a new BSC: %s\n", b.accepted ? "admitted" : "REFUSED");
	bsc_kill(&b);
	CHECK(wait_status("session: CLOSED", 5000));
}

static void x_eof(void)
{
	struct sup sup, *s = &sup;
	pid_t who[4];
	unsigned long wino[2];
	struct timespec t0;
	unsigned int i, n, ms;
	struct bsc b;
	char line[160];

	printf("X9: the BSC is SIGKILLed: each worker sees EOF on its data link by itself (DETACH delayed by 3 s)\n");
	sup_start(s, g_cfg, "x9", (const char *const[]){ "detach-delay-ms=3000", NULL });
	CHECK(wait_status("worker 1: READY", 15000));
	bsc_spawn(&b);
	CHECK(b.accepted);
	CHECK(wait_status("worker 1: READY, incarnation 1, attached", 5000));
	for (i = 0; i < 2; i++) {
		struct aworker_pool_worker_info dummy;
		(void)dummy;
		/* the BSC end: only the BSC process holds it (the Supervisor closed its copy) */
		n = holders(b.ino[i], who, 4);
		CHECK(n == 1 && who[0] == b.pid);
		/* the worker end: only one process holds it, not the Supervisor and not the BSC */
		wino[i] = peer_inode(b.ino[i]);
		CHECK(wino[i] != 0);
		n = holders(wino[i], who, 4);
		CHECK(n == 1 && who[0] != b.pid && who[0] != s->pid);
		printf("  worker %u: its data endpoint is held by %u process (the worker), the BSC end by the BSC only\n",
		       i, n);
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	bsc_kill(&b);
	WAIT_FOR(file_count(s->log, "detached itself from session") >= 2, 2500);
	ms = ms_since(&t0);
	for (i = 0; i < 2; i++) {
		snprintf(line, sizeof(line), "worker %u detached itself from session %u: data-eof", i, b.session);
		CHECK(file_has(s->log, line));
	}
	printf("  both workers reported data-eof %s (the delayed DETACH was not needed)\n",
	       ms < 2500 ? "within 2.5 s" : "TOO LATE");
	CHECK(ms < 2500);
	CHECK(wait_status("session: CLOSED", 3000));
	for (i = 0; i < 2; i++)
		CHECK(holders(wino[i], who, 4) == 0);
	printf("  afterwards nobody holds either end\n");

	printf("X9b: SESSION_OPEN while the last session is CLOSING (its DETACHes delayed): busy\n");
	{
		struct cli c, c2;
		struct dl d[2];
		struct msgb *msg;
		CHECK(full_session(&c, d));
		OSMO_ASSERT(aworker_ipc_enc_session_close(ctx, &msg, AWORKER_IPC_CC_NORMAL) == 0);
		aworker_ipc_link_send(c.link, msg, NULL);
		CHECK(wait_status("session: CLOSING", 2000));
		CHECK(cli_open(&c2, g_sock));
		session_open(&c2, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
		printf("  a new BSC: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason),
		       strstr(c2.text, "CLOSING") ? "the last session is CLOSING" : c2.text);
		CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BUSY && strstr(c2.text, "CLOSING"));
		cli_free(&c2);
		/* the delayed DETACHes end it */
		WAIT_FOR(d[0].closed && d[1].closed, 6000);
		CHECK(d[0].closed && d[1].closed);
		CHECK(wait_status("session: CLOSED", 3000));
		data_free(d);
		cli_free(&c);
		printf("  then the delayed DETACHes close the workers' data links: CLOSED\n");
	}
	sup_stop(s);
}

static void x_worker_loss(void)
{
	struct cli c;
	struct dl d[2];
	char t[AWORKER_IPC_MAX_TEXT + 1], *p;
	long wpid = 0;

	printf("X10: a worker lost: WORKER_DOWN for it only; its replacement is attached again\n");
	CHECK(full_session(&c, d));
	op(g_sock, AWORKER_IPC_OP_STATUS, 0, t, sizeof(t));
	p = strstr(t, "worker 1: READY, pid ");
	CHECK(p && sscanf(p + 21, "%ld", &wpid) == 1);
	if (wpid > 1)
		kill(wpid, SIGKILL);
	WAIT_FOR(c.n_down >= 1 && c.n_up >= 3, 8000);
	printf("  WORKER_DOWN: worker 0 %u, worker 1 %u (%s); WORKER_UP for worker 1 incarnation %u\n", c.down_count[0],
	       c.down_count[1], c.down_count[1] ? (c.down_reason[1] == AWORKER_IPC_DOWN_LOST ? "lost" : "other") : "-",
	       c.up_inc[1]);
	CHECK(c.down_count[0] == 0 && c.down_count[1] == 1 && c.down_reason[1] == AWORKER_IPC_DOWN_LOST);
	CHECK(c.n_up == 3 && c.up_inc[1] == 2);
	CHECK(!d[0].closed);
	CHECK(status_has("session: ACTIVE"));
	CHECK(wait_status("worker 1: READY, incarnation 2, attached", 5000));
	CHECK(status_has("worker 0: READY, incarnation 1, attached"));
	session_end(&c, d);
	CHECK(wait_status("session: CLOSED", 5000));
}

static void x_races(struct sup *s)
{
	struct cli c;
	struct dl d[2];
	struct msgb *msg;
	char t[AWORKER_IPC_MAX_TEXT + 1];
	unsigned int before;

	printf("X11: SESSION_CLOSE immediately followed by EOF: CLOSING entered once\n");
	before = 0;
	op(g_sock, AWORKER_IPC_OP_STATUS, 0, t, sizeof(t));
	CHECK(strstr(t, "CLOSING entered: ") && sscanf(strstr(t, "CLOSING entered: ") + 17, "%u", &before) == 1);
	CHECK(full_session(&c, d));
	OSMO_ASSERT(aworker_ipc_enc_session_close(ctx, &msg, AWORKER_IPC_CC_NORMAL) == 0);
	aworker_ipc_link_send(c.link, msg, NULL);
	pump_ms(1);
	cli_free(&c);
	WAIT_FOR(d[0].closed && d[1].closed, 5000);
	data_free(d);
	CHECK(wait_status("session: CLOSED", 5000));
	snprintf(t, sizeof(t), "CLOSING entered: %u", before + 1);
	CHECK(status_has(t));
	printf("  %s (was %u)\n", status_has(t) ? "exactly one more CLOSING" : "NOT exactly one", before);
}

static void x_fence_variants(void)
{
	static const char *const no_pidfd[] = { "no-pidfd", NULL };
	static const char *const no_signal[] = { "fence-signal=0", NULL };
	char t[AWORKER_IPC_MAX_TEXT + 1];
	struct sup s;
	struct bsc b;
	struct cli c2;
	int rc;

	printf("X12: fencing refused without a pidfd: the session is left as it is\n");
	sup_start(&s, g_cfg, "x12", no_pidfd);
	CHECK(wait_status("worker 1: READY", 15000));
	bsc_spawn(&b);
	CHECK(b.accepted);
	CHECK(status_has("fencing unavailable"));
	rc = op(g_sock, AWORKER_IPC_OP_FENCE_SESSION, 0, t, sizeof(t));
	printf("  FENCE_SESSION: rc %d (%s)\n", rc, rc == -ENOTSUP ? "not supported" : t);
	CHECK(rc == -ENOTSUP);
	CHECK(status_has("session: ACTIVE"));
	CHECK(kill(b.pid, 0) == 0);
	bsc_kill(&b);
	sup_stop(&s);

	printf("X13: fencing not confirmed: FENCE_FAILED, nothing admitted\n");
	sup_start(&s, g_cfg, "x13", no_signal);
	CHECK(wait_status("worker 1: READY", 15000));
	bsc_spawn(&b);
	CHECK(b.accepted);
	rc = op(g_sock, AWORKER_IPC_OP_FENCE_SESSION, 0, t, sizeof(t));
	printf("  FENCE_SESSION: rc %d (%s)\n", rc, rc == -ETIMEDOUT ? "FENCE_FAILED" : t);
	CHECK(rc == -ETIMEDOUT && strstr(t, "FENCE_FAILED"));
	CHECK(status_has("session: FENCING") && status_has("FENCE_FAILED"));
	CHECK(cli_open(&c2, g_sock));
	session_open(&c2, g_doc, g_doc_len, g_doc_len, AWORKER_IPC_SESSION_VERSION, true);
	printf("  a second BSC: %s (%s)\n", aworker_ipc_reject_reason_name(c2.reject_reason), c2.text);
	CHECK(c2.rejected && c2.reject_reason == AWORKER_IPC_REJ_BUSY && strstr(c2.text, "FENCE_FAILED"));
	cli_free(&c2);
	bsc_kill(&b);
	CHECK(wait_status("session: CLOSED", 5000));
	printf("  the BSC's end then closes it\n");
	sup_stop(&s);
}

static void x_shutdown(void)
{
	char t[AWORKER_IPC_MAX_TEXT + 1];
	struct sup s;
	struct cli c;
	struct dl d[2];
	struct stat st;
	int rc;

	printf("X14: operator RESET_WORKER and POOL_SHUTDOWN; the BSC is told\n");
	sup_start(&s, g_cfg, "x14", NULL);
	CHECK(wait_status("worker 1: READY", 15000));
	rc = op(g_sock, AWORKER_IPC_OP_RESET_WORKER, 0, t, sizeof(t));
	printf("  RESET_WORKER 0: rc %d (%s)\n", rc, t);
	CHECK(rc == -EBUSY);
	CHECK(full_session(&c, d));
	rc = op(g_sock, AWORKER_IPC_OP_POOL_SHUTDOWN, 0, t, sizeof(t));
	printf("  POOL_SHUTDOWN: rc %d (%s)\n", rc, t);
	CHECK(rc == 0);
	WAIT_FOR(c.got_close, 5000);
	printf("  the BSC got SESSION_CLOSE(%s)\n", c.got_close && c.close_reason == AWORKER_IPC_CC_SHUTDOWN
	       ? "shutdown" : "-");
	CHECK(c.got_close && c.close_reason == AWORKER_IPC_CC_SHUTDOWN);
	rc = sup_wait(&s, 15000);
	printf("  the Supervisor exits %d; socket %s\n", rc, stat(g_sock, &st) < 0 ? "removed" : "STILL THERE");
	CHECK(rc == 0 && stat(g_sock, &st) < 0);
	data_free(d);
	cli_free(&c);
}

int main(int argc, char **argv)
{
	struct sup s;
	char cmd[400];
	ssize_t n;

	ctx = talloc_named_const(NULL, 0, "aworker_session_test");
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_log_level(osmo_stderr_target, LOGL_FATAL);
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (argc == 5 && !strcmp(argv[1], "--bsc-client"))
		return bsc_client_main(argv[2], argv[3], atoi(argv[4]));
	if (argc < 2) {
		fprintf(stderr, "usage: %s PATH-TO-bsc-aworker\n", argv[0]);
		return 2;
	}
	g_bin = argv[1];
	n = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);
	OSMO_ASSERT(n > 0);
	g_self[n] = '\0';

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_session.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	snprintf(g_sock, sizeof(g_sock), "%s/s.sock", g_dir);
	snprintf(g_cfg, sizeof(g_cfg), "%s/osmo-bsc.cfg", g_dir);
	write_cfg(g_cfg, g_sock, NULL);

	/* The BSC's canonical document, built by this process from the same file as the Supervisor builds its own. */
	vty_info.tall_ctx = ctx;
	vty_init(&vty_info);
	logging_vty_add_cmds();
	OSMO_ASSERT(osmo_ss7_init() == 0);
	osmo_ss7_vty_init_asp(ctx);
	osmo_sccp_vty_init();
	aworker_pool_vty_init(ctx);
	OSMO_ASSERT(aworker_pool_supervisor_read_config(g_cfg) == 0);
	{
		char why[200];
		g_doc = aworker_pool_canon_document(ctx, &g_doc_len, why, sizeof(why));
		OSMO_ASSERT(g_doc);
	}
	log_set_log_level(osmo_stderr_target, LOGL_FATAL);

	x_socket();

	sup_start(&s, g_cfg, "main", NULL);
	CHECK(wait_status("worker 1: READY", 15000));
	print_status("STATUS before any session");
	x_admission(&s);
	x_unresponsive();
	x_fence();
	x_worker_loss();
	x_races(&s);
	sup_stop(&s);

	x_eof();

	x_fence_variants();
	x_shutdown();

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("logs kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

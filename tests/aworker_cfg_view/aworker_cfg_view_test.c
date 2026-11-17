/* Tests of the A-worker configuration views (inline worker configuration, decision D-S1) */
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

/* Part A: golden views and rule checks (no parser).
 * Part B: every generated view is parsed by the real libosmovty / libosmo-sigtran parsers, in a child process of its
 *         own (parsing an asp or listen node starts a transport), and the resulting objects are printed. Transports
 *         use TCP on 127.0.0.1 with a port that is free when the test starts, so nothing outside this host and no
 *         lab equipment is ever contacted; the child exits right after parsing, which closes everything.
 * Part C: the entry points osmo-bsc uses (prescan, read, VTY commands, config writer).
 * Part D: the source configuration is read once (S2-2 step 1). D4 links with -Wl,--wrap=fstat,--wrap=pread (see
 *         Makefile.am) to leave a single check of aworker_cfg_read_stable() in charge; D5 compares parsing the file
 *         by path (vty_read_config_file()) with parsing the buffer read once (aworker_pool_read_config()). */

#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/vty/buffer.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/misc.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

static void *ctx;
static char g_dir[256];
static unsigned int g_port;		/* free TCP port on 127.0.0.1, chosen at start */
static unsigned int g_port2;
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
	.name = "aworker_cfg_view_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

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

/* Write text to <g_dir>/<name>, replacing every "@P" by g_port and "@Q" by g_port2. \returns the path. */
static const char *put(const char *name, const char *text)
{
	static char path[8][512];
	static unsigned int next;
	char *p = path[next++ % 8];
	FILE *f;
	const char *s;

	snprintf(p, 512, "%s/%s", g_dir, name);
	f = fopen(p, "w");
	OSMO_ASSERT(f);
	for (s = text; *s; s++) {
		if (s[0] == '@' && (s[1] == 'P' || s[1] == 'Q')) {
			fprintf(f, "%u", s[1] == 'P' ? g_port : g_port2);
			s++;
		} else {
			fputc(*s, f);
		}
	}
	fclose(f);
	return p;
}

/* <g_dir>/<name> (no file is written). */
static const char *path_in_dir(const char *name)
{
	static char path[4][512];
	static unsigned int next;
	char *p = path[next++ % 4];

	snprintf(p, 512, "%s/%s", g_dir, name);
	return p;
}

/* Write text, as it is, over the file at path. */
static void put_path(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	OSMO_ASSERT(f);
	OSMO_ASSERT(fputs(text, f) >= 0);
	OSMO_ASSERT(fclose(f) == 0);
}

static struct aworker_cfg_model *model(const char *path)
{
	struct aworker_cfg_model *m;
	OSMO_ASSERT(aworker_cfg_model_build(ctx, path, &m) == 0);
	return m;
}

static void print_problems(const struct aworker_cfg_model *m, bool supervisor_mode)
{
	struct aworker_cfg_problem p[16];
	unsigned int n = aworker_cfg_model_check(m, supervisor_mode, p, ARRAY_SIZE(p)), i;

	printf("  check%s: %u problem(s)\n", supervisor_mode ? " (supervisor mode)" : "", n);
	for (i = 0; i < n && i < ARRAY_SIZE(p); i++)
		printf("    line %u: %s\n", p[i].lineno, p[i].msg);
}

static void print_view(const struct aworker_cfg_model *m, enum aworker_cfg_view_role role, unsigned int idx,
		       const char *label)
{
	size_t len;
	char *v = aworker_cfg_model_view(ctx, m, role, idx, &len), *line;
	unsigned int n = 0, lines = 0;
	const char *s;

	if (!v) {
		printf("  %s view: none\n", label);
		return;
	}
	for (s = v; *s; s++)
		lines += *s == '\n';
	if (len && v[len - 1] != '\n')
		lines++;
	printf("  %s view (%u lines; file %u lines):\n", label, lines, m->num_lines);
	CHECK(lines == m->num_lines);
	/* Print the kept lines with their numbers; empty lines are the removed ones (or blank in the file). */
	for (line = v; line && *line; ) {
		char *nl = strchr(line, '\n');
		n++;
		if (nl)
			*nl = '\0';
		if (*line)
			printf("    %2u|%s\n", n, line);
		line = nl ? nl + 1 : NULL;
	}
	talloc_free(v);
}

static void views(const char *name, const char *text, int num_workers_views, bool show_supervisor_check)
{
	const char *path = put(name, text);
	struct aworker_cfg_model *m = model(path);
	int i;

	printf("== %s\n", name);
	printf("  inline: %s\n", aworker_cfg_model_is_inline(m) ? "yes" : "no");
	print_problems(m, false);
	if (show_supervisor_check)
		print_problems(m, true);
	if (num_workers_views < 0) {
		talloc_free(m);
		return;
	}
	print_view(m, AWORKER_CFG_VIEW_BSC, 0, "BSC");
	print_view(m, AWORKER_CFG_VIEW_SUPERVISOR, 0, "Supervisor");
	for (i = 0; i < num_workers_views; i++) {
		char label[32];
		snprintf(label, sizeof(label), "worker %d", i);
		print_view(m, AWORKER_CFG_VIEW_WORKER, i, label);
	}
	talloc_free(m);
}

/***********************************************************************
 * Part A: golden views and checks
 ***********************************************************************/

static const char *cfg_inline_basic =
	"! two A links, each owned by a worker\n"
	"log stderr\n"
	" logging filter all 1\n"
	" logging level set-all notice\n"
	"line vty\n"
	" bind 127.0.0.1\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc0\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-a0 2905 0 m3ua\n"
	"  remote-ip 10.0.0.10\n"
	"  ! a comment inside the asp\n"
	"  local-ip 10.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"  no shutdown\n"
	" as as-a0 m3ua\n"
	"  asp asp-a0\n"
	"  routing-key 0 0.23.3\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" sccp-address msc0b\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-a1 2905 0 m3ua\n"
	"  remote-ip 10.0.0.10\n"
	"  no shutdown\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n"
	" restart-delay 5\n"
	"network\n"
	" network country code 001\n";

static void part_a(void)
{
	printf("Part A: views and rule checks\n");

	views("inline-basic", cfg_inline_basic, 2, true);

	/* Pool section first; tabs; mixed but consistent indentation; '!' inside a token; blank lines; the last
	 * section without a trailing newline. */
	views("layout",
	      "a-worker-pool\n"
	      "\tworkers 1\n"
	      "\tworker 0 cs7-instance 2\n"
	      "\n"
	      "log file /tmp/x.log\n"
	      "\tlogging level set-all info\n"
	      "cs7 instance 2\n"
	      "\t point-code 1.2.3\n"
	      "\t asp a2 2905 0 m3ua\n"
	      "\t \tdescription not!a!comment\n"
	      "\t \t! a comment\n"
	      "\n"
	      "\t \tremote-ip 10.0.0.2\n"
	      "\t no asp gone\n"
	      "\t listen m3ua 2906\n"
	      "\t \tlocal-ip 10.0.0.3",
	      1, false);

	views("bad-indent-in-owned",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      " asp a 2905 0 m3ua\n"
	      "  remote-ip 1.1.1.1\n"
	      "\t local-ip 2.2.2.2\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	views("transport-outside-owned",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "cs7 instance 5\n"
	      " point-code 1.2.5\n"
	      " asp stray 2905 0 m3ua\n"
	      "cs7 instance abc\n"
	      " listen m3ua 2905\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	views("owned-without-transport",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      " sccp-address x\n"
	      "  point-code 1.2.4\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	{
		char text[2048];
		char longdesc[600];
		memset(longdesc, 'x', sizeof(longdesc) - 1);
		longdesc[sizeof(longdesc) - 1] = '\0';
		snprintf(text, sizeof(text),
			 "cs7 instance 0\n"
			 " asp a 2905 0 m3ua\n"
			 "  description %s\n"
			 "a-worker-pool\n"
			 " workers 1\n"
			 " worker 0 cs7-instance 0\n", longdesc);
		views("overlong-line", text, -1, false);
	}

	views("exit-end",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "  exit\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n"
	      " end\n",
	      -1, false);

	views("duplicate-owned-section",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	views("both-forms-one-worker",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 config-file /etc/w0.cfg\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	views("mixed-pool",
	      "cs7 instance 1\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 2\n"
	      " worker 0 config-file /etc/w0.cfg\n"
	      " worker 1 cs7-instance 1\n",
	      -1, false);

	views("file-form-supervisor-mode",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 config-file /etc/w0.cfg\n",
	      -1, true);

	views("inline-supervisor-mode",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, true);

	views("missing-owned-section",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 2\n"
	      " worker 0 cs7-instance 0\n"
	      " worker 1 cs7-instance 7\n",
	      -1, false);

	views("two-workers-one-instance",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 2\n"
	      " worker 0 cs7-instance 0\n"
	      " worker 1 cs7-instance 0\n",
	      -1, false);

	views("protocol-conflict",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      " as b sua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	views("declaration-beyond-workers",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "cs7 instance 2\n"
	      " asp b 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n"
	      " worker 3 cs7-instance 2\n",
	      -1, false);

	/* A line indented less than the line before, but deeper than the level it returns to: the parser refuses it. */
	views("dedent-between-levels",
	      "cs7 instance 0\n"
	      "   point-code 1.2.3\n"
	      "  asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);

	/* The highest instance number the inline form accepts. */
	views("highest-instance",
	      "cs7 instance 15\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 15\n",
	      1, false);

	/* A declaration the file removes again ('no worker N cs7-instance'): not the inline form. */
	views("declaration-removed",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n"
	      " no worker 0 cs7-instance\n",
	      -1, false);

	/* A number the parser's <0-15> refuses ('0x'): the line declares nothing. */
	views("number-with-trailing-characters",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0x\n",
	      -1, false);

	/* Not the inline form: no views are used; the file form's own rule applies (Part C). */
	views("file-form",
	      "cs7 instance 0\n"
	      " point-code 1.2.3\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 config-file /etc/w0.cfg\n",
	      -1, false);

	views("pool-disabled",
	      "cs7 instance 0\n"
	      " asp a 2905 0 m3ua\n"
	      "a-worker-pool\n"
	      " workers 0\n"
	      " worker 0 cs7-instance 0\n",
	      -1, false);
}

/***********************************************************************
 * Part B: views parsed by the real parsers
 ***********************************************************************/

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
			if (sscanf(line, " %*s %*s %*s %7s %*s %*s %*s %*s %*s %lu", st, &inode) != 2)
				continue;
			if (strcmp(st, "0A"))
				continue;
			for (i = 0; i < n_inodes; i++) {
				if (inodes[i] == inode)
					count++;
			}
		}
		fclose(f);
	}
	return count;
}

/* The objects the parser created, as text (deterministic: no ports, no pids). */
static void facts(char *out, size_t len)
{
	static const char *const asps[] = { "asp-a0", "asp-a1", "asp-x" };
	static const char *const addrs[] = { "msc0", "msc0b" };
	struct osmo_strbuf sb = { .buf = out, .len = len };
	unsigned int i, j;

	out[0] = '\0';
	for (i = 0; i < AWORKER_CFG_VIEW_MAX_CS7; i++) {
		struct osmo_ss7_instance *inst = osmo_ss7_instance_find(i);
		if (!inst)
			continue;
		OSMO_STRBUF_PRINTF(sb, "    cs7 instance %u: point-code %s\n", i,
				   osmo_ss7_pointcode_print(inst, osmo_ss7_instance_get_primary_pc(inst)));
		for (j = OSMO_SS7_ASP_PROT_SUA; j < _NUM_OSMO_SS7_ASP_PROT; j++) {
			if (osmo_ss7_as_find_by_proto(inst, j))
				OSMO_STRBUF_PRINTF(sb, "      AS %s\n", osmo_ss7_asp_protocol_name(j));
		}
		for (j = 0; j < ARRAY_SIZE(asps); j++) {
			struct osmo_ss7_asp *asp = osmo_ss7_asp_find_by_name(inst, asps[j]);
			if (asp)
				OSMO_STRBUF_PRINTF(sb, "      ASP %s (%s)\n", osmo_ss7_asp_get_name(asp),
						   osmo_ss7_asp_protocol_name(osmo_ss7_asp_get_proto(asp)));
		}
	}
	for (j = 0; j < ARRAY_SIZE(addrs); j++) {
		struct osmo_sccp_addr a;
		struct osmo_ss7_instance *inst = osmo_sccp_addr_by_name(&a, addrs[j]);
		if (inst)
			OSMO_STRBUF_PRINTF(sb, "    sccp-address %s: cs7 instance %u, point-code %s\n", addrs[j],
					   osmo_ss7_instance_get_id(inst), osmo_ss7_pointcode_print(inst, a.pc));
	}
	OSMO_STRBUF_PRINTF(sb, "    listening sockets: %u\n", count_own_listen_sockets());
}

/* Parse text (a view or a whole file) in a child process. The child prints the parse result and the facts;
 * the parent returns them. Each parse gets a fresh process, so no state leaks between cases. */
static void parse_in_child(const char *text, size_t len, char *out, size_t outlen, bool via_read_config,
			   const char *path)
{
	int pp[2];
	pid_t pid;
	ssize_t n, got = 0;
	int status;

	OSMO_ASSERT(pipe(pp) == 0);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		char buf[4096];
		int rc;
		close(pp[0]);
		if (via_read_config) {
			rc = aworker_pool_read_config(path);
		} else {
			FILE *f = fmemopen((void *)text, len, "r");
			OSMO_ASSERT(f);
			rc = vty_read_config_filep(f, NULL);
			fclose(f);
		}
		snprintf(buf, sizeof(buf), "    parse: %s\n", rc == 0 ? "ok" : "refused");
		if (write(pp[1], buf, strlen(buf)) < 0) {}
		if (via_read_config) {
			snprintf(buf, sizeof(buf), "    VTY config file: %s\n",
				 host_config_file() ? "set (write file saves to it)" : "none (write file refuses)");
			if (write(pp[1], buf, strlen(buf)) < 0) {}
		}
		facts(buf, sizeof(buf));
		if (write(pp[1], buf, strlen(buf)) < 0) {}
		_exit(0);
	}
	close(pp[1]);
	while (got < (ssize_t)outlen - 1 && (n = read(pp[0], out + got, outlen - 1 - got)) > 0)
		got += n;
	out[got] = '\0';
	close(pp[0]);
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void parse_view(const struct aworker_cfg_model *m, enum aworker_cfg_view_role role, unsigned int idx,
		       char *out, size_t outlen)
{
	size_t len;
	char *v = aworker_cfg_model_view(ctx, m, role, idx, &len);
	OSMO_ASSERT(v);
	parse_in_child(v, len, out, outlen, false, NULL);
	talloc_free(v);
}

static void parse_file(const char *path, char *out, size_t outlen)
{
	FILE *f = fopen(path, "r");
	char text[8192];
	size_t len;

	OSMO_ASSERT(f);
	len = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	parse_in_child(text, len, out, outlen, false, NULL);
}

/* The real-parser configuration: only nodes this test installs (log, line vty, cs7, a-worker-pool). Transports
 * are TCP on 127.0.0.1 towards a port that was free when the test started. */
static const char *cfg_real =
	"log stderr\n"
	" logging level set-all notice\n"
	"line vty\n"
	" bind 127.0.0.1\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc0\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-a0 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"  no shutdown\n"
	" as as-a0 m3ua\n"
	"  asp asp-a0\n"
	"  routing-key 3 0.23.3\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" sccp-address msc0b\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" listen m3ua @Q tcp\n"
	"  local-ip 127.0.0.1\n"
	" asp asp-a1 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"  no shutdown\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n";

/* The same configuration in the legacy file form: the main file and the two worker files. */
static const char *cfg_legacy_main =
	"log stderr\n"
	" logging level set-all notice\n"
	"line vty\n"
	" bind 127.0.0.1\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc0\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" sccp-address msc0b\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n";
static const char *cfg_legacy_w0 =
	"log stderr\n"
	" logging level set-all notice\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc0\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-a0 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"  no shutdown\n"
	" as as-a0 m3ua\n"
	"  asp asp-a0\n"
	"  routing-key 3 0.23.3\n";
static const char *cfg_legacy_w1 =
	"log stderr\n"
	" logging level set-all notice\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" sccp-address msc0b\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" listen m3ua @Q tcp\n"
	"  local-ip 127.0.0.1\n"
	" asp asp-a1 @P 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"  no shutdown\n";

static void part_b(void)
{
	char bsc[4096], w0[4096], w1[4096], sup[4096], ref[4096];
	struct aworker_cfg_model *m;
	const char *path;

	printf("Part B: views parsed by the real VTY parsers\n");
	path = put("real.cfg", cfg_real);
	m = model(path);
	CHECK(aworker_cfg_model_check(m, false, NULL, 0) == 0);

	parse_view(m, AWORKER_CFG_VIEW_BSC, 0, bsc, sizeof(bsc));
	printf("  BSC view:\n%s", bsc);
	parse_view(m, AWORKER_CFG_VIEW_SUPERVISOR, 0, sup, sizeof(sup));
	printf("  Supervisor view:\n%s", sup);
	parse_view(m, AWORKER_CFG_VIEW_WORKER, 0, w0, sizeof(w0));
	printf("  worker 0 view:\n%s", w0);
	parse_view(m, AWORKER_CFG_VIEW_WORKER, 1, w1, sizeof(w1));
	printf("  worker 1 view:\n%s", w1);

	/* The same objects as the legacy form produces. */
	parse_file(put("legacy-main.cfg", cfg_legacy_main), ref, sizeof(ref));
	printf("  BSC view == legacy main file: %s\n", !strcmp(ref, bsc) ? "yes" : "NO");
	CHECK(!strcmp(ref, bsc));
	parse_file(put("legacy-w0.cfg", cfg_legacy_w0), ref, sizeof(ref));
	printf("  worker 0 view == legacy worker 0 file: %s\n", !strcmp(ref, w0) ? "yes" : "NO");
	CHECK(!strcmp(ref, w0));
	parse_file(put("legacy-w1.cfg", cfg_legacy_w1), ref, sizeof(ref));
	printf("  worker 1 view == legacy worker 1 file: %s\n", !strcmp(ref, w1) ? "yes" : "NO");
	CHECK(!strcmp(ref, w1));

	/* And the whole file, which no process may parse in pool mode, does create the transports here: proof that
	 * the BSC view's result is due to the view, not to the parser ignoring those nodes. */
	parse_file(path, ref, sizeof(ref));
	printf("  whole file (for comparison):\n%s", ref);
	talloc_free(m);

	/* A worker-only parse error: the BSC view parses (the line is not in it); the worker view is refused. */
	path = put("worker-parse-error.cfg",
		   "cs7 instance 0\n"
		   " point-code 0.23.3\n"
		   " asp asp-a0 @P 0 m3ua tcp\n"
		   "  remote-ip 127.0.0.1\n"
		   "  no-such-command\n"
		   "a-worker-pool\n"
		   " workers 1\n"
		   " worker 0 cs7-instance 0\n");
	m = model(path);
	parse_view(m, AWORKER_CFG_VIEW_BSC, 0, bsc, sizeof(bsc));
	printf("  worker-only parse error, BSC view:\n%s", bsc);
	parse_view(m, AWORKER_CFG_VIEW_WORKER, 0, w0, sizeof(w0));
	printf("  worker-only parse error, worker 0 view:\n%s", w0);
	talloc_free(m);

	/* A transport that cannot be set up: a local address not configured on this host (TEST-NET-1). */
	path = put("transport-failure.cfg",
		   "cs7 instance 0\n"
		   " point-code 0.23.3\n"
		   " asp asp-a0 @P 0 m3ua tcp\n"
		   "  remote-ip 127.0.0.1\n"
		   "  local-ip 192.0.2.1\n"
		   "  role asp\n"
		   "  transport-role client\n"
		   "  no shutdown\n"
		   "a-worker-pool\n"
		   " workers 1\n"
		   " worker 0 cs7-instance 0\n");
	m = model(path);
	parse_view(m, AWORKER_CFG_VIEW_WORKER, 0, w0, sizeof(w0));
	printf("  unusable local address, worker 0 view:\n%s", w0);
	talloc_free(m);
}

/***********************************************************************
 * Part C: the entry points osmo-bsc uses
 ***********************************************************************/

static int parse_text(const char *text)
{
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	int rc;

	OSMO_ASSERT(f);
	rc = vty_read_config_filep(f, NULL);
	fclose(f);
	return rc;
}

static void reset_pool_cfg(void)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int i;

	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		TALLOC_FREE(cfg->cfg_file[i]);
		cfg->has_cs7_instance[i] = false;
		cfg->cs7_instance[i] = 0;
	}
	cfg->num_workers = 0;
}

static void print_pool_section(const char *path)
{
	char line[256];
	bool in = false;
	FILE *f = fopen(path, "r");

	OSMO_ASSERT(f);
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "a-worker-pool", 13))
			in = true;
		else if (line[0] != ' ')
			in = false;
		if (in)
			printf("    %s", line);
	}
	fclose(f);
}

static void part_c(void)
{
	char out[4096];
	char wpath[512];

	printf("Part C: osmo-bsc entry points\n");

	/* Prescan: the D-S1 rules and the inline rules refuse; a consistent inline file passes; the file form keeps
	 * its own rule (asp in the main file refused); no pool, nothing restricted. */
	printf("  prescan inline consistent: %d\n", aworker_pool_prescan_config(put("c1.cfg", cfg_inline_basic)));
	printf("  prescan both forms on one worker: %d\n", aworker_pool_prescan_config(put("c2.cfg",
		"cs7 instance 0\n asp a 2905 0 m3ua\na-worker-pool\n workers 1\n worker 0 config-file /x\n"
		" worker 0 cs7-instance 0\n")));
	printf("  prescan file form with an asp in the main file: %d\n", aworker_pool_prescan_config(put("c3.cfg",
		"cs7 instance 0\n asp a 2905 0 m3ua\na-worker-pool\n workers 1\n worker 0 config-file /x\n")));
	printf("  prescan no pool, asp in the main file: %d\n", aworker_pool_prescan_config(put("c4.cfg",
		"cs7 instance 0\n asp a 2905 0 m3ua\n")));

	/* aworker_pool_read_config(): inline → BSC view, file not recorded; otherwise the whole file, recorded. */
	parse_in_child(NULL, 0, out, sizeof(out), true, put("real-read.cfg", cfg_real));
	printf("  read inline file:\n%s", out);
	parse_in_child(NULL, 0, out, sizeof(out), true, put("legacy-read.cfg", cfg_legacy_main));
	printf("  read file without a pool:\n%s", out);

	/* The VTY refuses the second form for the same worker, in either order. */
	reset_pool_cfg();
	printf("  VTY config-file then cs7-instance: %d\n", parse_text(
		"a-worker-pool\n workers 1\n worker 0 config-file /x\n worker 0 cs7-instance 0\n"));
	reset_pool_cfg();
	printf("  VTY cs7-instance then config-file: %d\n", parse_text(
		"a-worker-pool\n workers 1\n worker 0 cs7-instance 0\n worker 0 config-file /x\n"));
	reset_pool_cfg();
	printf("  VTY cs7-instance, no ..., config-file: %d\n", parse_text(
		"a-worker-pool\n workers 1\n worker 0 cs7-instance 0\n no worker 0 cs7-instance\n"
		" worker 0 config-file /x\n"));

	/* The config writer. */
	reset_pool_cfg();
	OSMO_ASSERT(parse_text("a-worker-pool\n workers 2\n worker 0 cs7-instance 3\n worker 1 cs7-instance 0\n") == 0);
	snprintf(wpath, sizeof(wpath), "%s/written.cfg", g_dir);
	OSMO_ASSERT(osmo_vty_write_config_file(wpath) == 0);
	printf("  written configuration:\n");
	print_pool_section(wpath);

	/* An inline declaration alone (no 'workers') is still written. */
	reset_pool_cfg();
	OSMO_ASSERT(parse_text("a-worker-pool\n worker 2 cs7-instance 5\n") == 0);
	OSMO_ASSERT(osmo_vty_write_config_file(wpath) == 0);
	printf("  written configuration, inline declaration only:\n");
	print_pool_section(wpath);
	reset_pool_cfg();
}

/***********************************************************************
 * Part D: the source configuration is read once (S2-2 step 1)
 ***********************************************************************/

/* Part D4: fstat() and pread(), as called by the A-worker library and by this test (linked with -Wl,--wrap=fstat
 * -Wl,--wrap=pread). Unarmed, they only pass the call on. A D4 case sets g_wrap.mode; the BEFORE_OPEN hook then arms
 * them for that one aworker_cfg_read_stable() call, so that every fstat() and pread() it makes is seen and logged, in
 * order, together with the hooks. */
int __real_fstat(int fd, struct stat *st);
ssize_t __real_pread(int fd, void *buf, size_t count, off_t offset);
int __wrap_fstat(int fd, struct stat *st);
ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset);

enum wrap_mode {
	WRAP_OFF,
	WRAP_SHORT_READ,		/* the file is really truncated right before the first pread() */
	WRAP_GROW_KEEP_TIMES,		/* at the second fstat(): really append, then report the first fstat()'s times */
	WRAP_KEEP_TIMES,		/* at the second fstat(): report the first fstat()'s times (control: no change) */
};

static struct {
	enum wrap_mode mode;
	bool armed;
	const char *path;
	off_t truncate_to;
	unsigned int n_fstat, n_pread, n_after_read;
	struct stat first;		/* what the first fstat() returned */
	struct stat real_second;	/* what the second fstat() really returned, before the times were replaced */
	char log[16][128];
	unsigned int n_log;
} g_wrap;

static void wrap_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wrap_log(const char *fmt, ...)
{
	va_list ap;

	OSMO_ASSERT(g_wrap.n_log < ARRAY_SIZE(g_wrap.log));
	va_start(ap, fmt);
	vsnprintf(g_wrap.log[g_wrap.n_log++], sizeof(g_wrap.log[0]), fmt, ap);
	va_end(ap);
}

ssize_t __wrap_pread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t rc;

	if (!g_wrap.armed)
		return __real_pread(fd, buf, count, offset);
	if (g_wrap.mode == WRAP_SHORT_READ && g_wrap.n_pread == 0) {
		/* A real short read: the file itself ends early when it is read. */
		OSMO_ASSERT(truncate(g_wrap.path, g_wrap.truncate_to) == 0);
		wrap_log("file truncated to %lld octets", (long long)g_wrap.truncate_to);
	}
	rc = __real_pread(fd, buf, count, offset);
	g_wrap.n_pread++;
	wrap_log("pread(offset %lld, %zu octets) = %zd", (long long)offset, count, rc);
	return rc;
}

int __wrap_fstat(int fd, struct stat *st)
{
	int rc, afd;

	if (!g_wrap.armed)
		return __real_fstat(fd, st);
	g_wrap.n_fstat++;
	if (g_wrap.n_fstat == 2 && (g_wrap.mode == WRAP_GROW_KEEP_TIMES || g_wrap.mode == WRAP_KEEP_TIMES)) {
		/* Only valid as the check after the read: the AFTER_READ hook must have been called right before. */
		OSMO_ASSERT(g_wrap.n_after_read == 1);
		if (g_wrap.mode == WRAP_GROW_KEEP_TIMES) {
			afd = open(g_wrap.path, O_WRONLY | O_APPEND | O_CLOEXEC);
			OSMO_ASSERT(afd >= 0 && write(afd, "!\n", 2) == 2);
			close(afd);
		}
		rc = __real_fstat(fd, st);
		OSMO_ASSERT(rc == 0);
		g_wrap.real_second = *st;
		/* As a file system with coarse timestamps could report it: the times of the first fstat(). */
		st->st_mtim = g_wrap.first.st_mtim;
		st->st_ctim = g_wrap.first.st_ctim;
		wrap_log("fstat #2 = %lld octets%s; mtime and ctime of fstat #1 reported", (long long)st->st_size,
			 g_wrap.mode == WRAP_GROW_KEEP_TIMES ? " (2 octets really appended first)" : "");
		return rc;
	}
	rc = __real_fstat(fd, st);
	if (g_wrap.n_fstat == 1 && rc == 0)
		g_wrap.first = *st;
	wrap_log("fstat #%u = %lld octets", g_wrap.n_fstat, rc == 0 ? (long long)st->st_size : -1LL);
	return rc;
}

/* What the D1 hook does between the read and the checks. */
static enum { HOOK_NONE, HOOK_TOUCH, HOOK_FLIP_BUFFER, HOOK_RENAME_OVER, HOOK_GROW, HOOK_SHRINK, HOOK_RETARGET,
	      HOOK_SWAP_TO_FIFO } g_hook;

static void stable_hook(enum aworker_cfg_read_stable_stage stage, const char *path, char *buf, size_t len)
{
	char tmp[600], c;
	int fd;

	if (g_wrap.mode != WRAP_OFF) {
		/* D4: arm the wrappers for this call, and log the hooks among the calls. */
		if (stage == AWORKER_CFG_READ_STABLE_BEFORE_OPEN) {
			g_wrap.armed = true;
			wrap_log("BEFORE_OPEN hook");
		} else {
			g_wrap.n_after_read++;
			wrap_log("AFTER_READ hook (%zu octets)", len);
		}
		return;
	}
	if (stage == AWORKER_CFG_READ_STABLE_BEFORE_OPEN) {
		if (g_hook == HOOK_SWAP_TO_FIFO) {
			/* The checked regular file is replaced by a FIFO before it is opened. */
			snprintf(tmp, sizeof(tmp), "%s.fifo", path);
			OSMO_ASSERT(mkfifo(tmp, 0600) == 0);
			OSMO_ASSERT(rename(tmp, path) == 0);
		}
		return;
	}
	switch (g_hook) {
	case HOOK_SWAP_TO_FIFO:
		break;
	case HOOK_RETARGET:
		/* path is a symlink: point it at another file. The file that was read is not touched at all. */
		snprintf(tmp, sizeof(tmp), "%s.link", path);
		OSMO_ASSERT(symlink(path_in_dir("stable-other.cfg"), tmp) == 0);
		OSMO_ASSERT(rename(tmp, path) == 0);
		break;
	case HOOK_NONE:
		break;
	case HOOK_TOUCH:
		/* Change the first octet and put it back: the contents end up identical, only the times change. */
		fd = open(path, O_WRONLY);
		OSMO_ASSERT(fd >= 0 && len > 0);
		c = buf[0] ^ 1;
		OSMO_ASSERT(pwrite(fd, &c, 1, 0) == 1);
		OSMO_ASSERT(pwrite(fd, buf, 1, 0) == 1);
		close(fd);
		break;
	case HOOK_FLIP_BUFFER:
		/* The file is untouched; what was read differs from it (only the second pass can tell). */
		buf[len / 2] ^= 1;
		break;
	case HOOK_RENAME_OVER:
		/* Another file with the same contents is renamed over the path. */
		snprintf(tmp, sizeof(tmp), "%s.new", path);
		fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		OSMO_ASSERT(fd >= 0 && write(fd, buf, len) == (ssize_t)len);
		close(fd);
		OSMO_ASSERT(rename(tmp, path) == 0);
		break;
	case HOOK_GROW:
		fd = open(path, O_WRONLY | O_APPEND);
		OSMO_ASSERT(fd >= 0 && write(fd, "!\n", 2) == 2);
		close(fd);
		break;
	case HOOK_SHRINK:
		OSMO_ASSERT(truncate(path, (off_t)(len - 1)) == 0);
		break;
	}
}

static const char *errname(int rc)
{
	switch (rc) {
	case 0: return "ok";
	case -ENOENT: return "-ENOENT";
	case -EINVAL: return "-EINVAL (not a regular file)";
	case -EFBIG: return "-EFBIG (too large)";
	case -ESTALE: return "-ESTALE (changed while read)";
	default: return "other error";
	}
}

static int read_stable(const char *path, size_t max, int hook, const char *expect)
{
	struct aworker_cfg_source *src = NULL;
	int rc;

	g_hook = hook;
	rc = aworker_cfg_read_stable(ctx, path, max, &src);
	g_hook = HOOK_NONE;
	if (rc == 0 && expect)
		CHECK(src->len == strlen(expect) && !memcmp(src->buf, expect, src->len) && src->buf[src->len] == '\0');
	CHECK((rc == 0) == (src != NULL));
	talloc_free(src);
	return rc;
}

/* inotify on one file: how often it is opened while fn runs (by anything, including the test's own writes). */
static unsigned int count_opens(const char *path, void (*fn)(const char *path))
{
	char ev[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	unsigned int opens = 0;
	ssize_t n;
	int in, wd;

	in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	OSMO_ASSERT(in >= 0);
	/* Close events too: inotify merges identical events that are next to each other in its queue, and an
	 * open is always followed by a close, so no two opens are ever adjacent. */
	wd = inotify_add_watch(in, path, IN_OPEN | IN_CLOSE);
	OSMO_ASSERT(wd >= 0);
	fn(path);
	while ((n = read(in, ev, sizeof(ev))) > 0) {
		char *p = ev;
		while (p < ev + n) {
			struct inotify_event *e = (struct inotify_event *)p;
			if (e->mask & IN_OPEN)
				opens++;
			p += sizeof(*e) + e->len;
		}
	}
	close(in);
	return opens;
}

static void read_stable_fifo(const char *path)
{
	struct aworker_cfg_source *src = NULL;
	OSMO_ASSERT(aworker_cfg_read_stable(ctx, path, AWORKER_CFG_SOURCE_MAX, &src) == -EINVAL);
}

static const char *stable_text = "cs7 instance 0\n point-code 0.23.3\n";

static void part_d1(void)
{
	char fifo[300];
	const char *path;
	int rc;

	printf("  D1: aworker_cfg_read_stable()\n");
	aworker_cfg_read_stable_set_test_hook(stable_hook);
	path = put("stable.cfg", stable_text);
	printf("    unchanged file: %s\n", errname(read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_NONE, stable_text)));
	printf("    size == limit: %s\n", errname(read_stable(path, strlen(stable_text), HOOK_NONE, stable_text)));
	printf("    size == limit + 1: %s\n", errname(read_stable(path, strlen(stable_text) - 1, HOOK_NONE, NULL)));
	printf("    missing file: %s\n", errname(read_stable(path_in_dir("no-such.cfg"), AWORKER_CFG_SOURCE_MAX,
							       HOOK_NONE, NULL)));
	printf("    directory: %s\n", errname(read_stable(g_dir, AWORKER_CFG_SOURCE_MAX, HOOK_NONE, NULL)));
	snprintf(fifo, sizeof(fifo), "%s/fifo.cfg", g_dir);
	OSMO_ASSERT(mkfifo(fifo, 0600) == 0);
	printf("    FIFO (does not block): %s; opened %u time(s)\n", errname(read_stable(fifo, AWORKER_CFG_SOURCE_MAX,
										   HOOK_NONE, NULL)),
	       count_opens(fifo, read_stable_fifo));
	unlink(fifo);

	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_TOUCH, NULL);
	printf("    written in place during the read, contents restored (times only): %s\n", errname(rc));
	CHECK(rc == -ESTALE);
	path = put("stable.cfg", stable_text);
	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_FLIP_BUFFER, NULL);
	printf("    read differs from the file (second pass only): %s\n", errname(rc));
	CHECK(rc == -ESTALE);
	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_RENAME_OVER, NULL);
	printf("    another file renamed over the path (same contents): %s\n", errname(rc));
	CHECK(rc == -ESTALE);
	path = put("stable.cfg", stable_text);
	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_GROW, NULL);
	printf("    grew during the read: %s\n", errname(rc));
	CHECK(rc == -ESTALE);
	path = put("stable.cfg", stable_text);
	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_SHRINK, NULL);
	printf("    shrank during the read: %s\n", errname(rc));
	CHECK(rc == -ESTALE);

	/* Only the path check sees this: the file read is untouched, the path names another one afterwards. */
	put("stable.cfg", stable_text);
	put("stable-other.cfg", stable_text);
	OSMO_ASSERT(symlink(path_in_dir("stable.cfg"), path_in_dir("stable-link.cfg")) == 0);
	rc = read_stable(path_in_dir("stable-link.cfg"), AWORKER_CFG_SOURCE_MAX, HOOK_RETARGET, NULL);
	printf("    symlink pointed at another file during the read (file read untouched): %s\n", errname(rc));
	CHECK(rc == -ESTALE);
	unlink(path_in_dir("stable-link.cfg"));

	/* Only the check after open() sees this: the path was a regular file when looked at, a FIFO when opened. */
	path = put("stable-swap.cfg", stable_text);
	rc = read_stable(path, AWORKER_CFG_SOURCE_MAX, HOOK_SWAP_TO_FIFO, NULL);
	printf("    replaced by a FIFO between the check and the open (does not block): %s\n", errname(rc));
	CHECK(rc == -EINVAL);
	unlink(path);
	aworker_cfg_read_stable_set_test_hook(NULL);
}

static const char *pc0(void)
{
	struct osmo_ss7_instance *inst = osmo_ss7_instance_find(0);
	return inst ? osmo_ss7_pointcode_print(inst, osmo_ss7_instance_get_primary_pc(inst)) : "(no cs7 instance 0)";
}

static int g_rc_prescan, g_rc_read;
static const char *g_rewrite;	/* written over the file between prescan and read, if set */

static void prescan_and_read(const char *path)
{
	g_rc_prescan = aworker_pool_prescan_config(path);
	if (g_rewrite)
		put_path(path, g_rewrite);
	g_rc_read = g_rc_prescan ? -1 : aworker_pool_read_config(path);
}

static void read_only(const char *path)
{
	g_rc_prescan = 0;
	g_rc_read = aworker_pool_read_config(path);
}

/* In a child: run the osmo-bsc entry points on text, report opens, results and the parsed point code. */
static void entry_points_case(const char *label, const char *text, const char *rewrite, bool with_prescan)
{
	pid_t pid;
	int status;

	printf("    %s:\n", label);
	fflush(stdout);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		const char *path = put("entry.cfg", text);
		unsigned int opens;

		failures = 0;
		g_rewrite = rewrite;
		opens = count_opens(path, with_prescan ? prescan_and_read : read_only);
		printf("      prescan %d, read %d; opened %u time(s)%s; cs7 instance 0 point-code %s;"
		       " VTY config file %s\n", g_rc_prescan, g_rc_read, opens,
		       rewrite ? " (one of them the test rewriting it)" : "", pc0(),
		       host_config_file() ? "set" : "none");
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static const char *d2_inline =
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" asp asp-x 2905 0 m3ua\n"
	"  remote-ip 127.0.0.1\n"
	"a-worker-pool\n"
	" workers 1\n"
	" worker 0 cs7-instance 0\n";
static const char *d2_inline_changed =
	"cs7 instance 0\n"
	" point-code 0.23.5\n"
	" asp asp-x 2905 0 m3ua\n"
	"  remote-ip 127.0.0.1\n"
	"a-worker-pool\n"
	" workers 1\n"
	" worker 0 cs7-instance 0\n";
static const char *d2_file_form =
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	"a-worker-pool\n"
	" workers 1\n"
	" worker 0 config-file /nonexistent/w0.cfg\n";
static const char *d2_file_form_changed =
	"cs7 instance 0\n"
	" point-code 0.23.5\n"
	"a-worker-pool\n"
	" workers 1\n"
	" worker 0 config-file /nonexistent/w0.cfg\n";
static const char *d2_no_pool = "cs7 instance 0\n point-code 0.23.3\n";
static const char *d2_no_pool_changed = "cs7 instance 0\n point-code 0.23.5\n";

static void part_d2(void)
{
	printf("  D2: one open and one buffer for a pool configuration; no pool: as before\n");
	entry_points_case("inline pool", d2_inline, NULL, true);
	entry_points_case("inline pool, file rewritten after the prescan (the buffer is parsed)", d2_inline,
			  d2_inline_changed, true);
	entry_points_case("inline pool, read without a prescan", d2_inline, NULL, false);
	entry_points_case("file-form pool", d2_file_form, NULL, true);
	entry_points_case("file-form pool, file rewritten after the prescan (the buffer is parsed)", d2_file_form,
			  d2_file_form_changed, true);
	entry_points_case("no pool (legacy: parsed from the path)", d2_no_pool, NULL, true);
	entry_points_case("no pool, file rewritten after the prescan (legacy: the new contents)", d2_no_pool,
			  d2_no_pool_changed, true);
}

/* A file of exactly size octets: text, then comment lines filling it up. */
static const char *put_sized(const char *name, const char *text, size_t size)
{
	const char *path = path_in_dir(name);
	size_t len = strlen(text), done;
	char line[1024];
	FILE *f = fopen(path, "w");

	OSMO_ASSERT(f && size >= len);
	OSMO_ASSERT(fwrite(text, 1, len, f) == len);
	memset(line, '#', sizeof(line));
	line[sizeof(line) - 1] = '\n';
	for (done = len; done < size; ) {
		size_t chunk = OSMO_MIN(sizeof(line), size - done);
		/* the last chunk ends with a newline too, unless it is a single octet */
		OSMO_ASSERT(fwrite(line + sizeof(line) - chunk, 1, chunk, f) == chunk);
		done += chunk;
	}
	OSMO_ASSERT(fclose(f) == 0);
	return path;
}

static void prescan_in_child(const char *label, const char *path, int hook)
{
	pid_t pid;
	int status;

	fflush(stdout);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		int rc;
		aworker_cfg_read_stable_set_test_hook(stable_hook);
		g_hook = hook;
		rc = aworker_pool_prescan_config(path);
		printf("    %s: prescan %d\n", label, rc);
		fflush(stdout);
		_exit(0);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void part_d3(void)
{
	char fifo[300];
	pid_t writer;
	int status;

	printf("  D3: a file not readable in one stable pass: refused with a pool, as before without\n");
	prescan_in_child("file-form pool, exactly 16 MiB",
			 put_sized("big.cfg", d2_file_form, AWORKER_CFG_SOURCE_MAX), HOOK_NONE);
	prescan_in_child("file-form pool, 16 MiB + 1",
			 put_sized("big.cfg", d2_file_form, AWORKER_CFG_SOURCE_MAX + 1), HOOK_NONE);
	prescan_in_child("no pool, 16 MiB + 1",
			 put_sized("big.cfg", d2_no_pool, AWORKER_CFG_SOURCE_MAX + 1), HOOK_NONE);
	unlink(path_in_dir("big.cfg"));
	prescan_in_child("inline pool, changed while read", put("changing.cfg", d2_inline), HOOK_GROW);
	prescan_in_child("no pool, changed while read", put("changing.cfg", d2_no_pool), HOOK_GROW);

	/* A FIFO: the stable read refuses it without blocking; the pool check reads what a writer sends. */
	snprintf(fifo, sizeof(fifo), "%s/pool.fifo", g_dir);
	OSMO_ASSERT(mkfifo(fifo, 0600) == 0);
	fflush(stdout);
	writer = fork();
	OSMO_ASSERT(writer >= 0);
	if (writer == 0) {
		int fd;
		/* Never hang the test: if nobody ever opens the FIFO for reading, give up. */
		alarm(20);
		fd = open(fifo, O_WRONLY);
		if (fd < 0 || write(fd, d2_inline, strlen(d2_inline)) != (ssize_t)strlen(d2_inline))
			_exit(1);
		close(fd);
		_exit(0);
	}
	prescan_in_child("inline pool from a FIFO", fifo, HOOK_NONE);
	OSMO_ASSERT(waitpid(writer, &status, 0) == writer);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(fifo);
}

/* One aworker_cfg_read_stable() call with the D4 wrappers armed; prints every fstat(), pread() and hook, in order. */
static int wrapped_read(enum wrap_mode mode, const char *path, struct aworker_cfg_source **src)
{
	unsigned int i;
	int rc;

	memset(&g_wrap, 0, sizeof(g_wrap));
	g_wrap.mode = mode;
	g_wrap.path = path;
	g_wrap.truncate_to = (off_t)(strlen(stable_text) / 2);
	*src = NULL;
	aworker_cfg_read_stable_set_test_hook(stable_hook);
	rc = aworker_cfg_read_stable(ctx, path, AWORKER_CFG_SOURCE_MAX, src);
	aworker_cfg_read_stable_set_test_hook(NULL);
	g_wrap.armed = false;
	g_wrap.mode = WRAP_OFF;
	for (i = 0; i < g_wrap.n_log; i++)
		printf("      %u. %s\n", i + 1, g_wrap.log[i]);
	printf("      result: %s; buffer returned: %s\n", errname(rc), *src ? "yes" : "no");
	return rc;
}

/* The calls logged by wrapped_read() must be exactly these. */
static void check_log(const char * const *expect, unsigned int n)
{
	unsigned int i;

	CHECK(g_wrap.n_log == n);
	for (i = 0; i < n && i < g_wrap.n_log; i++)
		CHECK(!strcmp(g_wrap.log[i], expect[i]));
}

/* SR6: a real short read. The function must give up at once: no growth probe, no second fstat(), and the AFTER_READ
 * hook -- the first thing that could use the buffer -- never gets the incomplete one. pread_full() asks for all len
 * octets at offset 0, gets the half that is left, asks for the rest at offset half and gets end-of-file: two pread()
 * calls, and nothing after them. */
static void d4_short_read(void)
{
	struct aworker_cfg_source *src;
	size_t len = strlen(stable_text), half = len / 2;
	char e[5][128];
	const char *expect[5] = { e[0], e[1], e[2], e[3], e[4] };
	int rc;

	printf("    short read (SR6): the file is truncated to %zu of its %zu octets right before it is read:\n", half, len);
	rc = wrapped_read(WRAP_SHORT_READ, put("stable.cfg", stable_text), &src);
	snprintf(e[0], sizeof(e[0]), "BEFORE_OPEN hook");
	snprintf(e[1], sizeof(e[1]), "fstat #1 = %zu octets", len);
	snprintf(e[2], sizeof(e[2]), "file truncated to %zu octets", half);
	snprintf(e[3], sizeof(e[3]), "pread(offset 0, %zu octets) = %zu", len, half);
	snprintf(e[4], sizeof(e[4]), "pread(offset %zu, %zu octets) = 0", half, len - half);
	check_log(expect, 5);
	CHECK(rc == -ESTALE);
	CHECK(src == NULL);
	CHECK(g_wrap.n_after_read == 0);
	CHECK(g_wrap.n_pread == 2);
	CHECK(g_wrap.n_fstat == 1);
	printf("      AFTER_READ hook called %u time(s); pread() %u time(s); fstat() %u time(s)\n",
	       g_wrap.n_after_read, g_wrap.n_pread, g_wrap.n_fstat);
	talloc_free(src);
}

/* SR1: the file grows after the growth probe and before the second fstat(), and that fstat() reports the times of the
 * first one, as a file system with coarse timestamps could. The growth probe already ran (and saw nothing), the times
 * are equal, the path still names the file read, and the octets read are still the file's first ones (the second pass
 * would match): the size comparison is the only check left to refuse. */
static void d4_grown_keep_times(void)
{
	struct aworker_cfg_source *src;
	size_t len = strlen(stable_text);
	char e[6][128], buf[64];
	const char *expect[6] = { e[0], e[1], e[2], e[3], e[4], e[5] };
	const char *path;
	struct stat st;
	ssize_t n;
	int rc, fd;

	printf("    grown after the growth probe, times unchanged (SR1):\n");
	path = put("stable.cfg", stable_text);
	rc = wrapped_read(WRAP_GROW_KEEP_TIMES, path, &src);
	snprintf(e[0], sizeof(e[0]), "BEFORE_OPEN hook");
	snprintf(e[1], sizeof(e[1]), "fstat #1 = %zu octets", len);
	snprintf(e[2], sizeof(e[2]), "pread(offset 0, %zu octets) = %zu", len, len);
	snprintf(e[3], sizeof(e[3]), "pread(offset %zu, 1 octets) = 0", len);
	snprintf(e[4], sizeof(e[4]), "AFTER_READ hook (%zu octets)", len);
	snprintf(e[5], sizeof(e[5]), "fstat #2 = %zu octets (2 octets really appended first); mtime and ctime of fstat #1"
		 " reported", len + 2);
	check_log(expect, 6);
	CHECK(rc == -ESTALE);
	CHECK(src == NULL);
	/* What every other check would have seen. */
	OSMO_ASSERT(stat(path, &st) == 0);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	OSMO_ASSERT(fd >= 0);
	n = read(fd, buf, sizeof(buf));
	close(fd);
	printf("      afterwards: the file has %lld octets, its first %zu unchanged: %s; the path names the file read"
	       " (device and inode): %s; the file read (inode) is the one that grew: %s\n", (long long)st.st_size, len,
	       n == (ssize_t)(len + 2) && !memcmp(buf, stable_text, len) ? "yes" : "no",
	       st.st_dev == g_wrap.first.st_dev && st.st_ino == g_wrap.first.st_ino ? "yes" : "no",
	       g_wrap.real_second.st_ino == g_wrap.first.st_ino && g_wrap.real_second.st_size == (off_t)(len + 2) ?
	       "yes" : "no");
	CHECK(st.st_size == (off_t)(len + 2));
	CHECK(n == (ssize_t)(len + 2) && !memcmp(buf, stable_text, len));
	CHECK(st.st_dev == g_wrap.first.st_dev && st.st_ino == g_wrap.first.st_ino);
	CHECK(g_wrap.real_second.st_ino == g_wrap.first.st_ino && g_wrap.real_second.st_size == (off_t)(len + 2));
	/* Depends on the file system and kernel, so not part of the expected output. */
	fprintf(stderr, "D4 SR1: the append really changed ctime on this host: %s\n",
		g_wrap.real_second.st_ctim.tv_sec != g_wrap.first.st_ctim.tv_sec
		|| g_wrap.real_second.st_ctim.tv_nsec != g_wrap.first.st_ctim.tv_nsec ? "yes" : "no");
	talloc_free(src);
}

/* Control for SR1: the same, without the growth. Reporting the first fstat()'s times alone refuses nothing; the second
 * pass runs and matches. */
static void d4_control_keep_times(void)
{
	struct aworker_cfg_source *src;
	size_t len = strlen(stable_text);
	char e[7][128];
	const char *expect[7] = { e[0], e[1], e[2], e[3], e[4], e[5], e[6] };
	int rc;

	printf("    control: times of the first fstat() reported, file unchanged:\n");
	rc = wrapped_read(WRAP_KEEP_TIMES, put("stable.cfg", stable_text), &src);
	snprintf(e[0], sizeof(e[0]), "BEFORE_OPEN hook");
	snprintf(e[1], sizeof(e[1]), "fstat #1 = %zu octets", len);
	snprintf(e[2], sizeof(e[2]), "pread(offset 0, %zu octets) = %zu", len, len);
	snprintf(e[3], sizeof(e[3]), "pread(offset %zu, 1 octets) = 0", len);
	snprintf(e[4], sizeof(e[4]), "AFTER_READ hook (%zu octets)", len);
	snprintf(e[5], sizeof(e[5]), "fstat #2 = %zu octets; mtime and ctime of fstat #1 reported", len);
	snprintf(e[6], sizeof(e[6]), "pread(offset 0, %zu octets) = %zu", len, len);
	check_log(expect, 7);
	CHECK(rc == 0 && src && src->len == len && !memcmp(src->buf, stable_text, len));
	talloc_free(src);
}

static void part_d4(void)
{
	printf("  D4: one check alone in charge (fstat() and pread() wrapped)\n");
	d4_short_read();
	d4_grown_keep_times();
	d4_control_keep_times();
}

/* D5: the file-form pool configuration parsed by path, as before step 1 (vty_read_config_file()), and from the buffer
 * read once (aworker_pool_prescan_config() + aworker_pool_read_config(), as osmo-bsc calls them), each in a child of
 * its own that starts from the same state. Compared: the return codes, the output of the VTY command 'show
 * running-config', everything written to stderr, the VTY's config file, and the point code of cs7 instance 0. */

static const char *put_bytes(const char *name, const char *data, size_t len)
{
	const char *path = path_in_dir(name);
	FILE *f = fopen(path, "w");

	OSMO_ASSERT(f && fwrite(data, 1, len, f) == len && fclose(f) == 0);
	return path;
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "r");
	char *s = NULL;
	size_t n = 0;
	char chunk[4096];
	size_t got;

	OSMO_ASSERT(f);
	while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
		s = talloc_realloc_size(ctx, s, n + got + 1);
		OSMO_ASSERT(s);
		memcpy(s + n, chunk, got);
		n += got;
	}
	fclose(f);
	if (!s)
		s = talloc_zero_size(ctx, 1);
	s[n] = '\0';
	*len = n;
	return s;
}

static void read_by_path(const char *path)
{
	g_rc_prescan = 0;
	g_rc_read = vty_read_config_file(path, NULL);
}

static void d5_child(const char *path, bool by_buffer, const char *tag)
{
	char p[600];
	pid_t pid;
	int status;

	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		struct vty *v;
		vector vline;
		char *out;
		FILE *f;
		int fd, rc_prescan = 0, rc, rc_show;
		unsigned int opens;

		snprintf(p, sizeof(p), "%s/d5-%s.err", g_dir, tag);
		fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		OSMO_ASSERT(fd >= 0 && dup2(fd, 2) == 2);
		close(fd);
		/* Opened once: parsed from the buffer read once. Twice (prescan, then parse by path): not taken for a pool
		 * configuration, parsed as before. */
		g_rewrite = NULL;
		opens = count_opens(path, by_buffer ? prescan_and_read : read_by_path);
		rc_prescan = g_rc_prescan;
		rc = g_rc_read;
		fflush(stderr);

		/* 'show running-config', executed as on a telnet VTY: its output collects in the VTY's buffer. */
		v = vty_new();
		OSMO_ASSERT(v);
		v->type = VTY_TERM;
		v->node = ENABLE_NODE;
		vline = cmd_make_strvec("show running-config");
		OSMO_ASSERT(vline);
		rc_show = cmd_execute_command(vline, v, NULL, 0);
		out = buffer_getstr(v->obuf);
		snprintf(p, sizeof(p), "%s/d5-%s.running", g_dir, tag);
		f = fopen(p, "w");
		OSMO_ASSERT(f && fputs(out ? out : "", f) >= 0 && fclose(f) == 0);

		snprintf(p, sizeof(p), "%s/d5-%s.info", g_dir, tag);
		f = fopen(p, "w");
		OSMO_ASSERT(f);
		fprintf(f, "prescan %d, read %d; file opened %u time(s); 'show running-config' %d; cs7 instance 0"
			" point-code %s\n%s", rc_prescan, rc, opens, rc_show, pc0(),
			host_config_file() ? host_config_file() : "(none)");
		OSMO_ASSERT(fclose(f) == 0);
		_exit(0);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static unsigned int count_lines(const char *s)
{
	unsigned int n = 0;

	for (; *s; s++)
		n += *s == '\n';
	return n;
}

/* Print text indented, without the VTY's carriage returns and with any other control character made visible. */
static void print_indented(const char *indent, const char *s, size_t len, bool skip_log_levels)
{
	size_t i;
	bool bol = true;
	unsigned int skipped = 0;

	if (!len)
		printf("%s(empty)\n", indent);
	for (i = 0; i < len; i++) {
		if (bol && skip_log_levels && len - i > 15 && !memcmp(s + i, " logging level ", 15)) {
			/* The categories are libosmocore's and osmo-bsc's: not shown, to keep this output stable. */
			while (i < len && s[i] != '\n')
				i++;
			skipped++;
			continue;
		}
		if (bol && skipped) {
			printf("%s (the ' logging level' lines are not shown)\n", indent);
			skipped = 0;
		}
		if (bol)
			printf("%s", indent);
		bol = false;
		if (s[i] == '\r')
			continue;
		if (s[i] == '\n') {
			printf("\n");
			bol = true;
		} else if ((unsigned char)s[i] < 0x20 && s[i] != '\t') {
			printf("\\x%02x", (unsigned char)s[i]);
		} else {
			putchar(s[i]);
		}
	}
	if (!bol)
		printf("\n");
}

static void d5_case(const char *label, const char *data, size_t len, bool print_running, bool print_err)
{
	const char *path = put_bytes("d5.cfg", data, len);
	char p[600], *run[2], *err[2], *info[2], *host[2];
	size_t run_len[2], err_len[2], info_len[2];
	static const char *tag[2] = { "path", "buffer" };
	int prescan[2], rd[2], show[2];
	unsigned int opens[2];
	char pc[2][64];
	unsigned int i;

	printf("    %s:\n", label);
	for (i = 0; i < 2; i++) {
		d5_child(path, i == 1, tag[i]);
		snprintf(p, sizeof(p), "%s/d5-%s.running", g_dir, tag[i]);
		run[i] = slurp(p, &run_len[i]);
		snprintf(p, sizeof(p), "%s/d5-%s.err", g_dir, tag[i]);
		err[i] = slurp(p, &err_len[i]);
		snprintf(p, sizeof(p), "%s/d5-%s.info", g_dir, tag[i]);
		info[i] = slurp(p, &info_len[i]);
		host[i] = strchr(info[i], '\n');
		OSMO_ASSERT(host[i]);
		*host[i]++ = '\0';
	}
	printf("      by path:   %s\n", info[0]);
	printf("      by buffer: %s\n", info[1]);
	printf("      VTY config file: %s\n", !strcmp(host[0], host[1]) ? (!strcmp(host[0], path) ? "the same, the file"
										: "the same, not the file")
										: "DIFFERENT");
	printf("      'show running-config': %s (%u lines)\n", run_len[0] == run_len[1]
	       && !memcmp(run[0], run[1], run_len[0]) ? "identical" : "DIFFERENT", count_lines(run[0]));
	printf("      stderr: %s (%zu octets)\n", err_len[0] == err_len[1] && !memcmp(err[0], err[1], err_len[0])
	       ? "identical" : "DIFFERENT", err_len[0]);
	/* The by-buffer path also runs the prescan, which the by-path child does not: its own result is shown above;
	 * everything else must be the same. */
	OSMO_ASSERT(sscanf(info[0], "prescan %d, read %d; file opened %u time(s); 'show running-config' %d;"
			   " cs7 instance 0 point-code %63[^\n]", &prescan[0], &rd[0], &opens[0], &show[0], pc[0]) == 5);
	OSMO_ASSERT(sscanf(info[1], "prescan %d, read %d; file opened %u time(s); 'show running-config' %d;"
			   " cs7 instance 0 point-code %63[^\n]", &prescan[1], &rd[1], &opens[1], &show[1], pc[1]) == 5);
	/* The file is opened once either way: by the path child to parse it, by the buffer child to read it once.
	 * Twice would mean the buffer child did not take the pool path, and the comparison would prove nothing. */
	CHECK(opens[0] == 1 && opens[1] == 1);
	CHECK(prescan[1] == 0);
	CHECK(rd[0] == rd[1]);
	CHECK(show[0] == 0 && show[1] == 0);
	CHECK(!strcmp(pc[0], pc[1]));
	CHECK(!strcmp(host[0], host[1]));
	CHECK(run_len[0] == run_len[1] && !memcmp(run[0], run[1], run_len[0]));
	CHECK(err_len[0] == err_len[1] && !memcmp(err[0], err[1], err_len[0]));
	if (print_running) {
		printf("      'show running-config' (both):\n");
		print_indented("        | ", run[0], run_len[0], true);
	}
	if (print_err || err_len[0] != err_len[1] || memcmp(err[0], err[1], err_len[0])) {
		for (i = 0; i < 2; i++) {
			printf("      stderr, by %s:\n", tag[i]);
			print_indented("        | ", err[i], err_len[i], false);
		}
	}
	if (run_len[0] != run_len[1] || memcmp(run[0], run[1], run_len[0])) {
		for (i = 0; i < 2; i++) {
			printf("      'show running-config', by %s:\n", tag[i]);
			print_indented("        | ", run[i], run_len[i], false);
		}
	}
	for (i = 0; i < 2; i++) {
		talloc_free(run[i]);
		talloc_free(err[i]);
		talloc_free(info[i]);
	}
	unlink(path);
}

#define D5_CASE(label, lit, print_running, print_err) \
	d5_case(label, lit, sizeof(lit) - 1, print_running, print_err)

static void part_d5(void)
{
	printf("  D5: file-form pool, parsed by path (as before) and from the buffer read once\n");
	D5_CASE("valid; comments, an empty line, the last line without a newline",
		"! a comment\n"
		"# another comment\n"
		"\n"
		"line vty\n"
		" bind 127.0.0.1\n"
		"cs7 instance 0\n"
		" point-code 0.23.3\n"
		"a-worker-pool\n"
		" workers 2\n"
		" worker 0 config-file /nonexistent/w0.cfg\n"
		" worker 1 config-file /nonexistent/w1.cfg", true, false);
	D5_CASE("invalid: an unknown command, more configuration after it",
		"cs7 instance 0\n"
		" point-code 0.23.3\n"
		"a-worker-pool\n"
		" workers 1\n"
		" worker 0 config-file /nonexistent/w0.cfg\n"
		"no-such-command\n"
		"cs7 instance 1\n"
		" point-code 0.23.4\n", false, true);
	D5_CASE("invalid: an argument the command does not accept",
		"cs7 instance 0\n"
		" point-code not-a-point-code\n"
		"a-worker-pool\n"
		" workers 1\n"
		" worker 0 config-file /nonexistent/w0.cfg\n", false, true);
	D5_CASE("NUL octets: inside a line, at the start of a line, inside a pool line, at the very end",
		"cs7 instance 0\n"
		" point-code 0.23.3\0 0.23.9 hidden\n"
		"\0 a whole line hidden\n"
		"a-worker-pool\n"
		" workers 1\0 junk\n"
		" worker 0 config-file /nonexistent/w0.cfg\n"
		"\0", true, true);
	D5_CASE("NUL octet right after the pool node's name",
		"cs7 instance 0\n"
		" point-code 0.23.3\n"
		"a-worker-pool\0 junk\n"
		" workers 1\n"
		" worker 0 config-file /nonexistent/w0.cfg\n", false, true);
}

/***********************************************************************
 * Part E: worker logging views (S2-2 step 2, decision D-2)
 ***********************************************************************/

/* text with every "@F" replaced by path */
static char *with_path(const char *text, const char *path)
{
	char *out = talloc_strdup(ctx, "");
	const char *s;

	for (s = text; *s; s++) {
		if (s[0] == '@' && s[1] == 'F') {
			out = talloc_strdup_append_buffer(out, path);
			s++;
		} else {
			out = talloc_strndup_append_buffer(out, s, 1);
		}
		OSMO_ASSERT(out);
	}
	return out;
}

/* The logging part of 'show running-config' (what libosmocore's own writer says about every log target), condensed:
 * the 'logging level <category> <level>' lines of a target are summarised as the level all categories have, or as
 * the majority level plus the exceptions. g_dir is printed as <dir>. */
static char *log_targets_now(void)
{
	struct vty *v = vty_new();
	vector vline = cmd_make_strvec("show running-config");
	char *run, *out = talloc_strdup(ctx, ""), *line, *save = NULL;
	char lvl[64][32], *lines[64];
	unsigned int n_lvl = 0, i, j;
	bool in_log = false;

	OSMO_ASSERT(v && vline && out);
	v->type = VTY_TERM;
	v->node = ENABLE_NODE;
	OSMO_ASSERT(cmd_execute_command(vline, v, NULL, 0) == CMD_SUCCESS);
	run = buffer_getstr(v->obuf);
	OSMO_ASSERT(run);

#define FLUSH_LEVELS() do { \
		if (n_lvl) { \
			/* the most frequent level */ \
			unsigned int best = 0, best_n = 0; \
			for (i = 0; i < n_lvl; i++) { \
				unsigned int c = 0; \
				for (j = 0; j < n_lvl; j++) \
					c += !strcmp(lvl[i], lvl[j]); \
				if (c > best_n) { best = i; best_n = c; } \
			} \
			out = talloc_asprintf_append_buffer(out, "         logging level: %s for %s categories\n", lvl[best], \
							     best_n == n_lvl ? "all" : "the other"); \
			for (i = 0; i < n_lvl; i++) { \
				if (strcmp(lvl[i], lvl[best])) \
					out = talloc_asprintf_append_buffer(out, "        %s\n", lines[i]); \
				talloc_free(lines[i]); \
			} \
			n_lvl = 0; \
		} \
	} while (0)

	for (line = strtok_r(run, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (!strncmp(line, "log ", 4)) {
			FLUSH_LEVELS();
			in_log = true;
		} else if (line[0] != ' ') {
			FLUSH_LEVELS();
			in_log = false;
			continue;
		}
		if (!in_log)
			continue;
		if (!strncmp(line, " logging level ", 15) && strrchr(line, ' ') && n_lvl < ARRAY_SIZE(lvl)) {
			osmo_strlcpy(lvl[n_lvl], strrchr(line, ' ') + 1, sizeof(lvl[0]));
			lines[n_lvl++] = talloc_strdup(ctx, line);
			continue;
		}
		{
			char *d = strstr(line, g_dir);
			if (d)
				out = talloc_asprintf_append_buffer(out, "        %.*s<dir>%s\n", (int)(d - line), line,
								     d + strlen(g_dir));
			else
				out = talloc_asprintf_append_buffer(out, "        %s\n", line);
		}
	}
	FLUSH_LEVELS();
#undef FLUSH_LEVELS
	return out;
}

/* In a child process: parse text with the real VTY parser and print the resulting log targets, and whether the log
 * file exists on disk. If dump is set, the targets are also written to it (for comparison by the caller). A missing
 * text (a view that was not made) is a test failure, reported here; the dump then says so, so that every comparison
 * with it fails too. */
static void log_parse_in_child(const char *label, const char *text, const char *logfile, const char *dump)
{
	pid_t pid;
	int status;

	printf("    %s:\n", label);
	if (!text) {
		FILE *f;
		printf("      FAIL: no view (aworker_cfg_model_view() returned NULL)\n");
		failures++;
		if (dump) {
			f = fopen(dump, "w");
			OSMO_ASSERT(f && fputs("(no view)\n", f) >= 0 && fclose(f) == 0);
		}
		return;
	}
	fflush(stdout);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		int rc, fd;
		char *t;
		FILE *f;

		/* Parser messages: not part of the expected output. */
		fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
		OSMO_ASSERT(fd >= 0 && dup2(fd, 2) == 2);
		close(fd);
		rc = parse_text(text);
		t = log_targets_now();
		printf("      parse: %d; log targets afterwards:\n%s      log file on disk: %s\n", rc, t,
		       access(logfile, F_OK) == 0 ? "yes" : "no");
		if (dump) {
			f = fopen(dump, "w");
			OSMO_ASSERT(f && fputs(t, f) >= 0 && fclose(f) == 0);
		}
		fflush(stdout);
		_exit(0);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	unlink(logfile);
}

/* E1: which target a 'logging ...' line configures, established with the real parser: the target of the 'log ...'
 * command it is indented below, and no other. */
static void part_e1(void)
{
	char lf[512];
	char *t;
	size_t la, lb;
	char *a, *b;

	/* A copy: path_in_dir() returns one of a few rotating buffers. */
	osmo_strlcpy(lf, path_in_dir("e1.log"), sizeof(lf));
	printf("  E1: logging target scoping in the real VTY parser\n");
	log_parse_in_child("nothing (the targets a process starts with)", "", lf, NULL);
	t = with_path("log file @F\n\tlogging level set-all info\n", lf);
	log_parse_in_child("'log file' with a tab-indented 'logging level set-all info' (as in the layout case)", t, lf,
			   NULL);
	talloc_free(t);
	t = with_path("log file @F\nlogging level set-all info\n", lf);
	log_parse_in_child("the same line not indented: refused, it exists only inside a 'log' node", t, lf, NULL);
	talloc_free(t);
	t = with_path("log stderr\n"
		      " logging level set-all notice\n"
		      " logging print category 1\n"
		      "log file @F\n"
		      " logging level set-all info\n"
		      " logging level rsl debug\n"
		      " logging timestamp 1\n"
		      " logging print file 1\n"
		      " logging filter all 1\n"
		      "log stderr\n"
		      " logging print level 1\n", lf);
	log_parse_in_child("stderr settings before and after a file target with its own settings", t, lf,
			   path_in_dir("e1-with-file.dump"));
	talloc_free(t);
	/* The same, with the file target's lines removed (empty lines, as a view has them). */
	log_parse_in_child("the same without the file target's six lines",
			   "log stderr\n"
			   " logging level set-all notice\n"
			   " logging print category 1\n"
			   "\n\n\n\n\n\n"
			   "log stderr\n"
			   " logging print level 1\n", lf, path_in_dir("e1-without-file.dump"));
	/* Everything but the file target must be the same. */
	a = slurp(path_in_dir("e1-with-file.dump"), &la);
	b = slurp(path_in_dir("e1-without-file.dump"), &lb);
	{
		char *fs = strstr(a, "        log file "), *end;
		OSMO_ASSERT(fs);
		end = strstr(fs + 1, "\n        log ");
		/* cut the file target's section out of a */
		memmove(fs, end ? end + 1 : fs + strlen(fs), strlen(end ? end + 1 : fs + strlen(fs)) + 1);
		printf("    targets other than the file, with and without the file target's lines: %s\n",
		       !strcmp(a, b) ? "identical" : "DIFFERENT");
		CHECK(!strcmp(a, b));
	}
	talloc_free(a);
	talloc_free(b);
	t = with_path("log file @F\n logging level set-all info\nno log file @F\n", lf);
	log_parse_in_child("'no log file' removes the file target", t, lf, NULL);
	talloc_free(t);
	log_parse_in_child("'no log file' for a file target that does not exist: refused",
			   "no log file /nonexistent/x.log\n", lf, NULL);
}

/* E2: the views of a file with every kind of log target, two workers. Text only: syslog, systemd-journal, gsmtap and
 * emscripten are not parsed here (they would reach the host's syslog / journal / a UDP port, or do not exist in
 * this build). */
static const char *cfg_logging_all =
	"! logging, every kind of target\n"
	"log stderr\n"
	" logging filter all 1\n"
	" logging level set-all notice\n"
	"log file /var/log/osmocom/osmo-bsc.log\n"
	" logging filter all 1\n"
	" logging level set-all info\n"
	" logging level rsl debug\n"
	" ! a comment inside the file target\n"
	" logging print file 1\n"
	" logging print category 1\n"
	" logging timestamp 1\n"
	"\n"
	"log syslog daemon\n"
	" logging level set-all error\n"
	"log systemd-journal raw\n"
	" logging level set-all notice\n"
	"log gsmtap 127.0.0.1\n"
	" logging level set-all fatal\n"
	"log alarms 98\n"
	" logging level set-all error\n"
	"log emscripten\n"
	" logging level set-all notice\n"
	"log file /var/log/osmocom/second.log blocking-io\n"
	"\tlogging level set-all debug\n"
	"no log file /var/log/osmocom/old.log\n"
	"no log syslog\n"
	"no log gsmtap\n"
	"no log stderr\n"
	"log stderr\n"
	" logging print level 1\n"
	"line vty\n"
	" bind 127.0.0.1\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" asp asp-a0 2905 0 m3ua\n"
	"  remote-ip 10.0.0.10\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" asp asp-a1 2905 0 m3ua\n"
	"  remote-ip 10.0.0.11\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n";

static void part_e2(void)
{
	printf("  E2: views of a file with every kind of log target, two workers\n");
	views("logging-all", cfg_logging_all, 2, false);
	/* A line the parser refuses (inconsistent indentation) inside a file target: the check reports it, and the
	 * worker view leaves it out with the rest of that target. */
	views("logging-bad-indent-in-file-target",
	      "log file /var/log/osmocom/osmo-bsc.log\n"
	      " logging level set-all info\n"
	      "\tlogging timestamp 1\n"
	      "log stderr\n"
	      " logging level set-all notice\n"
	      "a-worker-pool\n"
	      " workers 1\n"
	      " worker 0 cs7-instance 0\n", 1, false);
}

/* E3: the BSC view and each worker view of one file, really parsed: every target but the file targets ends up exactly
 * as the BSC has it, the file targets do not exist, and the workers never create the log file. */
static void part_e3(void)
{
	char lf[512];
	struct aworker_cfg_model *m;
	char *text, *view, *a, *b, *w;
	size_t la, lb;
	unsigned int i;

	/* A copy: path_in_dir() returns one of a few rotating buffers. */
	osmo_strlcpy(lf, path_in_dir("e3.log"), sizeof(lf));
	printf("  E3: the BSC view and the worker views, really parsed\n");
	text = with_path("log stderr\n"
			 " logging level set-all notice\n"
			 " logging print category 1\n"
			 "log file @F\n"
			 " logging level set-all info\n"
			 " logging level rsl debug\n"
			 " logging timestamp 1\n"
			 " logging print file 1\n"
			 " logging filter all 1\n"
			 "log alarms 98\n"
			 " logging level set-all error\n"
			 "log stderr\n"
			 " logging print level 1\n"
			 "a-worker-pool\n"
			 " workers 2\n"
			 " worker 0 cs7-instance 0\n"
			 " worker 1 cs7-instance 1\n", lf);
	m = model(put("e3.cfg", text));
	view = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_BSC, 0, NULL);
	log_parse_in_child("BSC view", view, lf, path_in_dir("e3-bsc.dump"));
	talloc_free(view);
	a = slurp(path_in_dir("e3-bsc.dump"), &la);
	{
		/* the BSC's targets without its file target */
		char *fs = strstr(a, "        log file "), *end;
		CHECK(fs);
		if (fs) {
			end = strstr(fs + 1, "\n        log ");
			memmove(fs, end ? end + 1 : fs + strlen(fs), strlen(end ? end + 1 : fs + strlen(fs)) + 1);
		}
	}
	for (i = 0; i < 2; i++) {
		char label[64], dump[64];
		view = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, i, NULL);
		snprintf(label, sizeof(label), "worker %u view", i);
		snprintf(dump, sizeof(dump), "e3-w%u.dump", i);
		log_parse_in_child(label, view, lf, path_in_dir(dump));
		talloc_free(view);
		b = slurp(path_in_dir(dump), &lb);
		w = strstr(b, "log file");
		printf("    worker %u: file target: %s; every other target as the BSC has it: %s\n", i,
		       w ? "PRESENT" : "none", !strcmp(a, b) ? "yes" : "NO");
		CHECK(!w);
		CHECK(!strcmp(a, b));
		talloc_free(b);
	}
	talloc_free(a);
	talloc_free(m);
	talloc_free(text);

	/* 'no log file': fine for the BSC, which has the target; a worker would fail on it without the target. */
	text = with_path("log file @F\n"
			 " logging level set-all info\n"
			 "no log file @F\n"
			 "log stderr\n"
			 " logging level set-all info\n"
			 "a-worker-pool\n"
			 " workers 1\n"
			 " worker 0 cs7-instance 0\n", lf);
	m = model(put("e3-no.cfg", text));
	view = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_BSC, 0, NULL);
	log_parse_in_child("'no log file', BSC view", view, lf, NULL);
	talloc_free(view);
	view = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, 0, NULL);
	log_parse_in_child("'no log file', worker 0 view", view, lf, NULL);
	talloc_free(view);
	talloc_free(m);
	talloc_free(text);
}

/* E4: the defence in depth. aworker_cfg_view_log_file_line() on its own, and aworker_cfg_model_view() refusing a
 * worker view with a 'log file' line even when the model claims there is none. */
static void part_e4(void)
{
	static const struct { const char *label, *text; } t[] = {
		{ "empty", "" },
		{ "stderr only", "log stderr\n logging level set-all info\n" },
		{ "commented out", "! log file /x.log\n# log file /x.log\n" },
		{ "'log file' on line 2", "log stderr\nlog file /x.log\n" },
		{ "'no log file', indented", "a-worker-pool\n no log file /x.log\n" },
		{ "'log file' indented below another node", "line vty\n\tlog file /x.log\n" },
		{ "tab between the keywords", "log\tfile /x.log\n" },
		{ "'logfile' (another word)", "logfile /x.log\n" },
		{ "'log files' (another word)", "log files /x.log\n" },
	};
	struct aworker_cfg_model *m;
	unsigned int i;
	char *v;

	printf("  E4: no 'log file' in a worker view, whatever the model says\n");
	for (i = 0; i < ARRAY_SIZE(t); i++)
		printf("    %s: line %u\n", t[i].label, aworker_cfg_view_log_file_line(t[i].text, strlen(t[i].text)));

	m = model(put("e4.cfg", "log file /x.log\n"
				" logging level set-all info\n"
				"log stderr\n"
				"a-worker-pool\n"
				" workers 1\n"
				" worker 0 cs7-instance 0\n"));
	v = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, 0, NULL);
	printf("    worker view from the model: %s\n", v ? (aworker_cfg_view_log_file_line(v, strlen(v)) ?
								"WITH a 'log file' line" : "made, no 'log file'")
							   : "none");
	CHECK(v && !aworker_cfg_view_log_file_line(v, strlen(v)));
	talloc_free(v);
	/* A model whose flags are wrong: the view would contain the file target. */
	for (i = 0; i < m->num_lines; i++)
		m->line[i].log_file = false;
	v = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, 0, NULL);
	printf("    worker view from a model that does not mark the file target: %s\n", v ? "MADE" : "none (refused)");
	CHECK(!v);
	talloc_free(v);
	v = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_BSC, 0, NULL);
	printf("    BSC view from that model: %s\n", v && aworker_cfg_view_log_file_line(v, strlen(v)) == 1 ?
	       "made, with the file target (the BSC keeps it)" : "WRONG");
	CHECK(v && aworker_cfg_view_log_file_line(v, strlen(v)) == 1);
	talloc_free(v);
	talloc_free(m);
}

/* E5: the model's D-2 marks themselves, line by line (not only what the views make of them): log_file decides
 * "file target" for 'log file' and 'no log file'; no_log marks every 'no log <target>'. */
static void part_e5(void)
{
	static const struct {
		const char *text;	/* followed by the pool section; the first line is the one checked */
		unsigned int line;	/* which line of text to check (1: the first) */
		bool log_file, no_log;
	} t[] = {
		{ "no log file /x.log\n", 1, true, true },
		{ "no  log\tfile /x.log\n", 1, true, true },
		{ "no log stderr\n", 1, false, true },
		{ "no log syslog\n", 1, false, true },
		{ "no log gsmtap\n", 1, false, true },
		{ "log file /x.log\n logging level set-all info\n", 1, true, false },
		{ "log file /x.log\n logging level set-all info\n", 2, true, false },
		{ "log file /x.log\n ! a comment\n", 2, true, false },
		{ "log stderr\n logging level set-all info\n", 1, false, false },
		{ "log stderr\n logging level set-all info\n", 2, false, false },
		{ "no logging level force-all\n", 1, false, false },
		{ "nolog file /x.log\n", 1, false, false },
		{ "! no log file /x.log\nlog stderr\n", 1, false, false },
		{ "log file /x.log\nlog stderr\n", 2, false, false },
		{ "no log file /x.log\nlog stderr\n", 2, false, false },
	};
	unsigned int i;

	printf("  E5: the model's marks (log_file, no_log)\n");
	for (i = 0; i < ARRAY_SIZE(t); i++) {
		char text[512], shown[128], *p;
		struct aworker_cfg_model *m;
		const struct aworker_cfg_line *l;
		const char *s, *e;
		unsigned int n;

		snprintf(text, sizeof(text), "%sa-worker-pool\n workers 1\n worker 0 cs7-instance 0\n", t[i].text);
		OSMO_ASSERT(aworker_cfg_model_build_buf(ctx, "e5", text, strlen(text), &m) == 0);
		OSMO_ASSERT(t[i].line >= 1 && t[i].line <= m->num_lines);
		l = &m->line[t[i].line - 1];
		/* the line checked, with its tab shown */
		for (s = t[i].text, n = 1; n < t[i].line; s = strchr(s, '\n') + 1, n++)
			;
		e = strchr(s, '\n');
		for (p = shown; s < e && p < shown + sizeof(shown) - 3; s++) {
			if (*s == '\t') {
				*p++ = '\\';
				*p++ = 't';
			} else {
				*p++ = *s;
			}
		}
		*p = '\0';
		printf("    '%s': log_file %d, no_log %d%s\n", shown, l->log_file, l->no_log,
		       l->log_file == t[i].log_file && l->no_log == t[i].no_log ? "" : "   <-- WRONG");
		CHECK(l->log_file == t[i].log_file);
		CHECK(l->no_log == t[i].no_log);
		talloc_free(m);
	}
}

static void part_e(void)
{
	printf("Part E: worker logging views (D-2)\n");
	part_e1();
	part_e2();
	part_e3();
	part_e4();
	part_e5();
}

static void part_d(void)
{
	printf("Part D: the source configuration is read once\n");
	part_d1();
	part_d2();
	part_d3();
	part_d4();
	part_d5();
}

/***********************************************************************
 * Part F: the worker-view check (S2-2 step 3, decision D-13, gate V-4)
 ***********************************************************************/

/* Check text as worker idx's view and print the outcome. \returns the number of problems. */
static unsigned int check_view(const char *label, const char *text, size_t len, unsigned int idx)
{
	struct aworker_cfg_problem p[16];
	unsigned int n = aworker_cfg_check_worker_view(text, len, idx, p, ARRAY_SIZE(p)), i;

	printf("    %s (worker %u): %s\n", label, idx, n ? "REFUSED" : "accepted");
	for (i = 0; i < n && i < ARRAY_SIZE(p); i++)
		printf("      line %u: %s\n", p[i].lineno, p[i].msg);
	return n;
}

/* The same, expecting a refusal by the rule whose message contains why, naming line expect_line (0: a problem not
 * tied to a line). Another rule refusing the text does not count. */
static void expect_refused_len(const char *label, const char *text, size_t len, unsigned int idx,
			       unsigned int expect_line, const char *why)
{
	struct aworker_cfg_problem p[16];
	unsigned int n, i;
	bool named = false;

	n = check_view(label, text, len, idx);
	CHECK(n > 0);
	n = aworker_cfg_check_worker_view(text, len, idx, p, ARRAY_SIZE(p));
	for (i = 0; i < n && i < ARRAY_SIZE(p); i++)
		named |= p[i].lineno == expect_line && strstr(p[i].msg, why);
	if (!named)
		printf("      (expected: line %u, '%s')\n", expect_line, why);
	CHECK(named);
}

static void expect_refused(const char *label, const char *text, unsigned int idx, unsigned int expect_line,
			   const char *why)
{
	expect_refused_len(label, text, strlen(text), idx, expect_line, why);
}

static void expect_accepted(const char *label, const char *text, unsigned int idx)
{
	CHECK(check_view(label, text, strlen(text), idx) == 0);
}

/* F1: every worker view the builder makes from the valid inline files of this test passes. */
static void part_f1(void)
{
	static const struct { const char *name; const char **text; unsigned int workers; } cfgs[] = {
		{ "inline-basic", &cfg_inline_basic, 2 },
		{ "real", &cfg_real, 2 },
		{ "logging-all", &cfg_logging_all, 2 },
		{ "d2-inline", &d2_inline, 1 },
	};
	unsigned int c, i;

	printf("  F1: the builder's worker views pass the check\n");
	for (c = 0; c < ARRAY_SIZE(cfgs); c++) {
		struct aworker_cfg_model *m = model(put("f1.cfg", *cfgs[c].text));
		for (i = 0; i < cfgs[c].workers; i++) {
			size_t len;
			char label[64], *v = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, i, &len);
			OSMO_ASSERT(v);
			snprintf(label, sizeof(label), "%s", cfgs[c].name);
			CHECK(check_view(label, v, len, i) == 0);
			talloc_free(v);
		}
		talloc_free(m);
	}
}

/* A valid view of worker 0 (lines 1-10); the cases below add to it or change it. */
#define F_VIEW \
	"log stderr\n" \
	" logging level set-all notice\n" \
	"a-worker-pool\n" \
	" workers 2\n" \
	" worker 0 cs7-instance 0\n" \
	" worker 1 cs7-instance 1\n" \
	"cs7 instance 0\n" \
	" point-code 0.23.3\n" \
	" asp asp-x 2905 0 m3ua\n" \
	"  remote-ip 10.0.0.10\n"

/* F2: one case per rule. */
static void part_f2(void)
{
	char *big;
	size_t len;

	printf("  F2: one case per rule\n");
	expect_accepted("valid", F_VIEW, 0);
	expect_accepted("valid, with comments, blank lines and the other targets' forms",
			"! worker 0\n\n" F_VIEW "no log stderr\nlog stderr\n logging print level 1\n! end of view\n", 0);

	/* Logging (D-2 and the nesting of log targets). */
	expect_refused("'log file'", F_VIEW "log file /x.log\n logging level set-all info\n", 0, 11, "never writes the BSC's log files");
	expect_refused("'no log file'", F_VIEW "no log file /x.log\n", 0, 11, "never writes the BSC's log files");
	expect_refused("'log file' indented below 'log stderr'",
		       "log stderr\n log file /x.log\n logging level set-all notice\n"
		       "a-worker-pool\n workers 1\n worker 0 cs7-instance 0\n"
		       "cs7 instance 0\n asp asp-x 2905 0 m3ua\n", 0, 2, "configured at the top level only");
	expect_refused("'log stderr' indented below the cs7 instance", F_VIEW " log stderr\n", 0, 11, "configured at the top level only");
	expect_refused("'logging ...' at the top level", F_VIEW "logging level set-all info\n", 0, 11, "is not part of a worker view");
	expect_refused("'logging ...' below the cs7 instance", F_VIEW " logging level set-all info\n", 0, 11, "outside a 'log <target>' section");

	/* cs7 instances and transports. */
	expect_refused("another worker's cs7 instance, with a transport",
		       F_VIEW "cs7 instance 1\n point-code 0.23.4\n asp asp-y 2905 0 m3ua\n  remote-ip 10.0.0.11\n",
		       0, 11, "is not worker 0's");
	expect_refused("another cs7 instance, configuration only", F_VIEW "cs7 instance 5\n point-code 0.23.9\n", 0,
		       11, "is not worker 0's");
	expect_refused("a cs7 instance that is not a valid number", F_VIEW "cs7 instance 99\n", 0, 11, "not a valid instance number");
	expect_refused("another cs7 command", F_VIEW "cs7 something\n", 0, 11, "is not part of a worker view");
	expect_refused("worker 1's view, checked as worker 0",
		       "a-worker-pool\n workers 2\n worker 0 cs7-instance 0\n worker 1 cs7-instance 1\n"
		       "cs7 instance 1\n point-code 0.23.4\n asp asp-y 2905 0 m3ua\n", 0, 5, "is not worker 0's");
	expect_refused("own cs7 instance missing",
		       "a-worker-pool\n workers 1\n worker 0 cs7-instance 0\n", 0, 3, "has no 'cs7 instance 0' section");
	expect_refused("own cs7 instance twice", F_VIEW "cs7 instance 0\n point-code 0.23.3\n", 0, 11, "appears more than once");
	expect_refused("own cs7 instance without a transport",
		       "a-worker-pool\n workers 1\n worker 0 cs7-instance 0\ncs7 instance 0\n point-code 0.23.3\n",
		       0, 4, "would own nothing");
	expect_refused("a transport below another command than its cs7 instance",
		       "a-worker-pool\n workers 1\n worker 0 cs7-instance 0\ncs7 instance 0\n asp asp-x 2905 0 m3ua\n"
		       "line vty\n asp asp-z 2905 0 m3ua\n", 0, 6, "is not part of a worker view");

	/* The worker's declaration. */
	expect_refused("no 'a-worker-pool' section", "cs7 instance 0\n asp asp-x 2905 0 m3ua\n", 0, 0, "has no 'a-worker-pool' section");
	expect_refused("worker not declared (worker 2, 'workers 2')", F_VIEW, 2, 3, "does not declare worker 2");
	expect_refused("worker index beyond the largest pool", F_VIEW, AWORKER_CFG_VIEW_MAX_WORKERS, 3, "does not declare worker 16");
	expect_refused("worker declared with 'config-file' only",
		       "a-worker-pool\n workers 1\n worker 0 config-file /w0.cfg\ncs7 instance 0\n"
		       " asp asp-x 2905 0 m3ua\n", 0, 3, "not declared with the inline form");
	expect_refused("worker declared in both forms",
		       "a-worker-pool\n workers 1\n worker 0 config-file /w0.cfg\n worker 0 cs7-instance 0\n"
		       "cs7 instance 0\n asp asp-x 2905 0 m3ua\n", 0, 4, "is declared twice");

	/* Sections a worker view never has. */
	expect_refused("'line vty' (a BSC section)", F_VIEW "line vty\n bind 127.0.0.1\n", 0, 11, "is not part of a worker view");
	expect_refused("'network' (a BSC section)", F_VIEW "network\n network country code 001\n", 0, 11, "is not part of a worker view");
	expect_refused("'msc 0' (a BSC section)", F_VIEW "msc 0\n cs7-instance-a 0\n", 0, 11, "is not part of a worker view");

	/* Structure. */
	expect_refused("'exit' below the cs7 instance", F_VIEW " exit\n", 0, 11, "'exit' / 'end'");
	expect_refused("'exit' in a log target", "log stderr\n exit\n" "a-worker-pool\n workers 1\n"
		       " worker 0 cs7-instance 0\ncs7 instance 0\n asp asp-x 2905 0 m3ua\n", 0, 2, "'exit' / 'end'");
	expect_refused("'end' at the top level", F_VIEW "end\n", 0, 11, "'exit' / 'end'");
	expect_refused("inconsistent indentation", F_VIEW "\t point-code 0.23.3\n", 0, 11, "inconsistent indentation");
	{
		char line[VTY_BUFSIZ + 64];
		memset(line, 'x', sizeof(line));
		memcpy(line, "  description ", 14);
		line[sizeof(line) - 2] = '\n';
		line[sizeof(line) - 1] = '\0';
		big = talloc_asprintf(ctx, "%s%s", F_VIEW, line);
		expect_refused("a line longer than the parser reads at once", big, 0, 11, "longer than");
		talloc_free(big);
	}

	/* Size (D-5). */
	expect_refused("empty", "", 0, 0, "is empty");
	/* F_VIEW padded with comment lines to exactly the limit, then one octet more. */
	big = talloc_size(ctx, AWORKER_CFG_SNAPSHOT_MAX + 2);
	OSMO_ASSERT(big);
	len = strlen(F_VIEW);
	memcpy(big, F_VIEW, len);
	while (len < AWORKER_CFG_SNAPSHOT_MAX) {
		size_t chunk = OSMO_MIN((size_t)100, AWORKER_CFG_SNAPSHOT_MAX - len);
		memset(big + len, '#', chunk);
		big[len + chunk - 1] = '\n';
		len += chunk;
	}
	big[len] = '\0';
	OSMO_ASSERT(len == AWORKER_CFG_SNAPSHOT_MAX);
	CHECK(check_view("exactly AWORKER_CFG_SNAPSHOT_MAX octets", big, len, 0) == 0);
	big[len] = '#';
	big[len + 1] = '\0';
	expect_refused_len("AWORKER_CFG_SNAPSHOT_MAX + 1 octets", big, len + 1, 0, 0, "more than the 1048576");
	CHECK(aworker_cfg_check_worker_view(big, len + 1, 0, NULL, 0) == 1);
	talloc_free(big);
}

/* F3: the check comes before any parse. In a child, as the worker will do it (S2-2 step 6): check; parse only if
 * the check passes; then print what exists. A refused view leaves no cs7 instance, no ASP and no socket. */
static void check_then_parse_in_child(const char *label, const char *text, unsigned int idx)
{
	int pp[2];
	pid_t pid;
	int status;
	char out[4096];
	ssize_t n, got = 0;

	printf("    %s (worker %u):\n", label, idx);
	OSMO_ASSERT(pipe(pp) == 0);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		struct aworker_cfg_problem p[16];
		unsigned int np = aworker_cfg_check_worker_view(text, strlen(text), idx, p, ARRAY_SIZE(p));
		char buf[4096];

		close(pp[0]);
		if (np) {
			snprintf(buf, sizeof(buf), "      check: %u problem(s), first on line %u; not parsed\n", np,
				 p[0].lineno);
		} else {
			FILE *f = fmemopen((void *)text, strlen(text), "r");
			int rc;
			OSMO_ASSERT(f);
			rc = vty_read_config_filep(f, NULL);
			fclose(f);
			snprintf(buf, sizeof(buf), "      check: ok; parse: %s\n", rc == 0 ? "ok" : "refused");
		}
		if (write(pp[1], buf, strlen(buf)) < 0) {}
		{
			unsigned int k, inst = 0;
			for (k = 0; k < AWORKER_CFG_VIEW_MAX_CS7; k++)
				inst += osmo_ss7_instance_find(k) != NULL;
			snprintf(buf, sizeof(buf), "      cs7 instances afterwards: %u\n", inst);
			if (write(pp[1], buf, strlen(buf)) < 0) {}
		}
		facts(buf, sizeof(buf));
		if (write(pp[1], buf, strlen(buf)) < 0) {}
		_exit(0);
	}
	close(pp[1]);
	while (got < (ssize_t)sizeof(out) - 1 && (n = read(pp[0], out + got, sizeof(out) - 1 - got)) > 0)
		got += n;
	out[got] = '\0';
	close(pp[0]);
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	printf("%s", out);
}

static void part_f3(void)
{
	struct aworker_cfg_model *m = model(put("f3.cfg", cfg_real));
	char *w0, *w1, *t;

	printf("  F3: refused views are never parsed: no cs7 instance, no ASP, no socket\n");
	w0 = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, 0, NULL);
	w1 = aworker_cfg_model_view(ctx, m, AWORKER_CFG_VIEW_WORKER, 1, NULL);
	OSMO_ASSERT(w0 && w1);
	check_then_parse_in_child("worker 1's view", w1, 1);
	check_then_parse_in_child("worker 1's view, given to worker 0", w1, 0);
	t = talloc_asprintf(ctx, "%slog file %s\n", w1, path_in_dir("f3.log"));
	check_then_parse_in_child("worker 1's view with a 'log file'", t, 1);
	CHECK(access(path_in_dir("f3.log"), F_OK) != 0);
	talloc_free(t);
	t = talloc_asprintf(ctx, "%s exit\n", w1);
	check_then_parse_in_child("worker 1's view with an 'exit'", t, 1);
	talloc_free(t);
	/* worker 0's view with worker 1's cs7 instance (its listen and asp nodes) added */
	t = talloc_asprintf(ctx, "%s%s", w0, strstr(w1, "cs7 instance 1\n"));
	check_then_parse_in_child("worker 0's view with worker 1's cs7 instance added", t, 0);
	talloc_free(t);
	talloc_free(w0);
	talloc_free(w1);
	talloc_free(m);
}

static void part_f(void)
{
	printf("Part F: the worker-view check (D-13)\n");
	part_f1();
	part_f2();
	part_f3();
}

int main(int argc, char **argv)
{
	char cmd[300];

	ctx = talloc_named_const(NULL, 0, "aworker_cfg_view_test");
	osmo_init_logging2(ctx, &test_log_info);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	vty_info.tall_ctx = ctx;
	vty_init(&vty_info);
	logging_vty_add_cmds();
	OSMO_ASSERT(osmo_ss7_init() == 0);
	osmo_ss7_vty_init_asp(ctx);
	osmo_sccp_vty_init();
	aworker_pool_vty_init(ctx);
	setvbuf(stdout, NULL, _IOLBF, 0);

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_cfg_view.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	g_port = free_port();
	do {
		g_port2 = free_port();
	} while (g_port2 == g_port);

	if (argc > 1) {
		/* Single cases, by name, e.g. "aworker_cfg_view_test D4-SR6 D4-SR1". Without arguments: everything. */
		static const struct { const char *name; void (*fn)(void); } cases[] = {
			{ "D4-SR6", d4_short_read },
			{ "D4-SR1", d4_grown_keep_times },
			{ "D4-control", d4_control_keep_times },
			{ "D5", part_d5 },
			{ "E1", part_e1 },
			{ "E2", part_e2 },
			{ "E3", part_e3 },
			{ "E4", part_e4 },
			{ "E5", part_e5 },
			{ "F1", part_f1 },
			{ "F2", part_f2 },
			{ "F3", part_f3 },
		};
		int a;
		unsigned int c;

		for (a = 1; a < argc; a++) {
			for (c = 0; c < ARRAY_SIZE(cases); c++) {
				if (!strcmp(argv[a], cases[c].name))
					break;
			}
			if (c == ARRAY_SIZE(cases)) {
				printf("unknown case '%s'\n", argv[a]);
				failures++;
				continue;
			}
			printf("%s:\n", cases[c].name);
			cases[c].fn();
		}
	} else {
		part_a();
		part_b();
		part_c();
		part_d();
		part_e();
		part_f();
	}

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

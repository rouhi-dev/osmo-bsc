/* Tests of the A-worker Supervisor's configuration (S2-3): aworker_pool_supervisor_read_config() */
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

/* C: one main configuration file per case, read by aworker_pool_supervisor_read_config() in a child process of its own
 * (the pool configuration, the VTY and the log targets are per process). The child initialises what the Supervisor
 * initialises -- logging with the shared categories, the VTY node set for parsing (no VTY server) -- reads the file,
 * and reports to this process what it found: the result, the pool configuration, whether any cs7 instance exists
 * (the Supervisor parses none), and the log targets created. What it printed on stderr is checked for the reason.
 * Linked against libaworker only (no BSC). */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

static char g_dir[256];
static unsigned int failures;

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
	.name = "aworker_supervisor_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

static void path_of(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/%s", g_dir, name);
}

static void write_file(const char *name, const char *text)
{
	char p[300];
	FILE *f;

	path_of(p, sizeof(p), name);
	f = fopen(p, "w");
	OSMO_ASSERT(f && fputs(text, f) >= 0 && fclose(f) == 0);
}

static char *slurp(const char *path)
{
	static char buf[16384];
	ssize_t n = 0, r;
	int fd = open(path, O_RDONLY);

	buf[0] = '\0';
	if (fd < 0)
		return buf;
	while (n < (ssize_t)sizeof(buf) - 1 && (r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0)
		n += r;
	buf[n] = '\0';
	close(fd);
	return buf;
}

/* The child: initialise as the Supervisor does, read, report. Never returns. */
static void child(const char *cfg)
{
	struct aworker_pool_cfg *pc;
	void *ctx = talloc_named_const(NULL, 0, "supervisor");
	char log_path[300];
	unsigned int i, cs7 = 0;
	int rc;

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

	rc = aworker_pool_supervisor_read_config(cfg);
	pc = aworker_pool_cfg();
	for (i = 0; i < 16; i++)
		cs7 += osmo_ss7_instance_find(i) != NULL;
	path_of(log_path, sizeof(log_path), "supervisor.log");
	printf("    result %s; supervisor mode %s; workers %u", rc == 0 ? "0" : rc == -EINVAL ? "-EINVAL" : "other",
	       aworker_pool_supervisor_mode() ? "yes" : "no", pc->num_workers);
	for (i = 0; i < pc->num_workers && i < 16; i++)
		printf("%s%u->cs7 %u", i ? ", " : " (", i, pc->has_cs7_instance[i] ? pc->cs7_instance[i] : 99);
	printf("%s; restart-delay %u; cs7 instances in this process %u; log file target %s\n",
	       pc->num_workers ? ")" : "", pc->restart_delay, cs7,
	       log_target_find(LOG_TGT_TYPE_FILE, log_path) ? "yes" : "no");
	fflush(stdout);
	_exit(rc == 0 ? 0 : 1);
}

/* Run a case: the file is cfg_text, or none if NULL (missing file). expect_rc 0 or 1; why: a text the refusal must
 * print on stderr (NULL: nothing to check). */
static void c_case(const char *label, const char *cfg_text, int expect_rc, const char *why)
{
	char cfg[300], err[300];
	int status, fd;
	pid_t pid;

	path_of(cfg, sizeof(cfg), "osmo-bsc.cfg");
	path_of(err, sizeof(err), "stderr.txt");
	unlink(cfg);
	if (cfg_text)
		write_file("osmo-bsc.cfg", cfg_text);
	printf("  %s:\n", label);
	fflush(stdout);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		OSMO_ASSERT(fd >= 0 && dup2(fd, 2) == 2);
		close(fd);
		child(cfg);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == expect_rc);
	if (why) {
		bool said = strstr(slurp(err), why) != NULL;
		printf("    says: %s\n", said ? why : "(NOT the expected reason)");
		CHECK(said);
		if (!said)
			printf("    stderr was: %s\n", slurp(err));
	}
}

/* An inline pool of two workers; @LOG@ is replaced by a 'log file' path in the test directory; BSC-only sections
 * (line vty, e1_input, network, msc) are present, as in a real osmo-bsc.cfg. */
static const char *cfg_ok =
	"log stderr\n"
	" logging filter all 1\n"
	" logging level set-all notice\n"
	"log file @LOG@\n"
	" logging filter all 1\n"
	" logging level set-all info\n"
	"line vty\n"
	" no login\n"
	"e1_input\n"
	" e1_line 0 driver ipa\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc-a\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-w0 2905 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" asp asp-w1 2905 0 m3ua tcp\n"
	"  remote-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n"
	" restart-delay 3\n"
	"network\n"
	" network country code 001\n"
	" mobile network code 01\n"
	"msc 0\n"
	" msc-addr msc-a\n";

/* cfg_ok with the first occurrence of 'from' replaced by 'to' (talloc'ed). */
static char *variant(const char *from, const char *to)
{
	char log_path[300], *orig, *base, *out, *at;

	path_of(log_path, sizeof(log_path), "supervisor.log");
	orig = talloc_strdup(NULL, cfg_ok);
	at = strstr(orig, "@LOG@");
	*at = '\0';
	base = talloc_asprintf(NULL, "%s%s%s", orig, log_path, at + 5);
	talloc_free(orig);
	if (!from)
		return base;
	at = strstr(base, from);
	OSMO_ASSERT(at);
	*at = '\0';
	out = talloc_asprintf(NULL, "%s%s%s", base, to, at + strlen(from));
	talloc_free(base);
	return out;
}

int main(int argc, char **argv)
{
	char cmd[300];
	char *t;

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_supervisor.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("C: the Supervisor's configuration (aworker_pool_supervisor_read_config())\n");

	t = variant(NULL, NULL);
	c_case("inline pool of two workers, with BSC-only sections", t, 0, NULL);
	talloc_free(t);
	printf("  (only the log targets and 'a-worker-pool' are parsed: no cs7 instance in the Supervisor; its 'log file'"
	       " target exists)\n");

	t = variant(" worker 0 cs7-instance 0\n worker 1 cs7-instance 1\n",
		    " worker 0 config-file /etc/w0.cfg\n worker 1 config-file /etc/w1.cfg\n");
	/* A file-form pool may not have transport nodes in the main file: drop them, so only D-S1 is at stake. */
	{
		char *u = t, *a;
		while ((a = strstr(u, " asp asp-w"))) {
			char *e = strstr(a, "  transport-role client\n") + strlen("  transport-role client\n");
			memmove(a, e, strlen(e) + 1);
		}
	}
	c_case("file form ('worker N config-file FILE')", t, 1, "the Supervisor accepts only the inline form");
	talloc_free(t);

	t = variant(" worker 1 cs7-instance 1\n", " worker 1 config-file /etc/w1.cfg\n");
	c_case("both forms in one pool", t, 1, "all workers of a pool use the same form");
	talloc_free(t);

	t = variant("a-worker-pool\n workers 2\n worker 0 cs7-instance 0\n worker 1 cs7-instance 1\n restart-delay 3\n",
		    "");
	c_case("no 'a-worker-pool' section", t, 1, "it configures no A-worker pool");
	talloc_free(t);

	t = variant(" workers 2\n worker 0 cs7-instance 0\n worker 1 cs7-instance 1\n", " workers 0\n");
	c_case("'workers 0'", t, 1, "it configures no A-worker pool");
	talloc_free(t);

	/* Also without instance 1's transport: otherwise the asp in an instance no worker owns is refused first. */
	t = variant(" worker 1 cs7-instance 1\n", "");
	{
		char *a = strstr(t, " asp asp-w1");
		char *e = strstr(a, "  transport-role client\n") + strlen("  transport-role client\n");
		memmove(a, e, strlen(e) + 1);
	}
	c_case("'workers 2' with only worker 0 declared", t, 1, "worker 1 has no 'worker 1 cs7-instance <N>'");
	talloc_free(t);

	t = variant(" restart-delay 3\n", " restart-delay 3\n on-worker-failure shutdown\n");
	c_case("'on-worker-failure shutdown' (D-S8)", t, 1, "'on-worker-failure shutdown' is not allowed for the"
	       " Supervisor (D-S8)");
	talloc_free(t);

	t = variant(" restart-delay 3\n", " restart-delay 3\n on-worker-failure restart\n");
	c_case("'on-worker-failure restart'", t, 0, NULL);
	talloc_free(t);

	c_case("no such file", NULL, 1, "cannot read it: No such file or directory");

	t = variant("cs7 instance 0\n", "cs7 instance 5\n asp asp-x 2905 0 m3ua tcp\n  remote-ip 127.0.0.1\ncs7 instance 0\n");
	c_case("an asp in a cs7 instance no worker owns", t, 1, "cs7 instance 5");
	talloc_free(t);

	t = variant(" restart-delay 3\n", " restart-delay soon\n");
	c_case("an invalid 'a-worker-pool' command", t, 1, "cannot be parsed");
	talloc_free(t);

	t = variant(" logging level set-all info\n", " logging level set-all info\n logging nonsense\n");
	c_case("an invalid command in a log target", t, 1, "cannot be parsed");
	talloc_free(t);

	if (!failures) {
		snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
		if (system(cmd)) {}
	} else {
		printf("files kept in %s\n", g_dir);
	}
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

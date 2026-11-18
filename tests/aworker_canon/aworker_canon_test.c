/* Tests of the canonical configuration document (CD-1, D-S5; S2-5): aworker_canon.h */
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

/* N: every document is built in a child process of its own from one osmo-bsc.cfg, by one of the two readers that
 * will build it in production: the Supervisor's (aworker_pool_supervisor_read_config(), view: log + a-worker-pool)
 * or osmo-bsc's inline read (aworker_pool_read_config(), the BSC view, which parses the cs7 instances without their
 * transport nodes). The child writes the document to a file; this process compares documents with
 * aworker_canon_compare(), as the Supervisor will at SESSION_OPEN. */

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

#include <osmocom/bsc/aworker_canon.h>
#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

static char g_dir[256];
static unsigned int failures;
static void *g_ctx;

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
	.name = "aworker_canon_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

enum reader { SUPERVISOR, BSC };

/* Only sections both readers can parse in this test (the BSC's own nodes -- network, msc, ... -- are not installed
 * here; part P/C exclusion of BSC-only sections is shown with 'log' and with a cs7 instance no worker owns). */
static const char *base =
	"! a comment\n"
	"log stderr\n"
	" logging level set-all notice\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" asp asp-w0 2905 0 m3ua\n"
	"  remote-ip 127.0.0.1\n"
	"  local-ip 127.0.0.1\n"
	"  role asp\n"
	"  transport-role client\n"
	"cs7 instance 1\n"
	" point-code 0.23.4\n"
	" sccp-address msc1\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-w1 2905 0 m3ua\n"
	"  remote-ip 127.0.0.2\n"
	"  role asp\n"
	"  transport-role client\n"
	"cs7 instance 5\n"
	" point-code 0.23.6\n"
	"a-worker-pool\n"
	" workers 2\n"
	" worker 0 cs7-instance 0\n"
	" worker 1 cs7-instance 1\n";

/* base with one substring replaced (old must occur exactly once), talloc'ed. */
static char *variant(const char *old, const char *new)
{
	const char *p;

	if (!old)
		return talloc_strdup(g_ctx, base);
	p = strstr(base, old);
	OSMO_ASSERT(p && !strstr(p + 1, old));
	return talloc_asprintf(g_ctx, "%.*s%s%s", (int)(p - base), base, new, p + strlen(old));
}

static void path_of(char *out, size_t len, const char *name)
{
	snprintf(out, len, "%s/%s", g_dir, name);
}

/* The child: initialise as the reader's process does, read, write the document (or "NONE <why>") to out. */
static void child(enum reader r, const char *cfg, const char *out)
{
	void *ctx = talloc_named_const(NULL, 0, "canon-child");
	char why[256];
	size_t len = 0;
	char *doc;
	FILE *f;
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

	if (r == SUPERVISOR)
		rc = aworker_pool_supervisor_read_config(cfg);
	else
		rc = aworker_pool_prescan_config(cfg) ?: aworker_pool_read_config(cfg);
	f = fopen(out, "w");
	OSMO_ASSERT(f);
	if (rc < 0) {
		fprintf(f, "NONE the configuration was refused (%d)", rc);
	} else {
		doc = aworker_pool_canon_document(ctx, &len, why, sizeof(why));
		if (doc) {
			OSMO_ASSERT(fwrite(doc, 1, len, f) == len);
		} else {
			fprintf(f, "NONE %s", why);
		}
	}
	OSMO_ASSERT(fclose(f) == 0);
	_exit(0);
}

/* Build the document of cfg_text with reader r. \returns it (talloc'ed), *len set. */
static char *build(enum reader r, const char *cfg_text, size_t *len)
{
	char cfg[300], out[300], err[300];
	char *buf;
	int status, fd;
	pid_t pid;
	FILE *f;
	struct stat st;

	path_of(cfg, sizeof(cfg), "osmo-bsc.cfg");
	path_of(out, sizeof(out), "doc.txt");
	path_of(err, sizeof(err), "stderr.txt");
	f = fopen(cfg, "w");
	OSMO_ASSERT(f && fputs(cfg_text, f) >= 0 && fclose(f) == 0);
	unlink(out);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		fd = open(err, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		OSMO_ASSERT(fd >= 0 && dup2(fd, 2) == 2);
		close(fd);
		child(r, cfg, out);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	OSMO_ASSERT(stat(out, &st) == 0);
	buf = talloc_size(g_ctx, st.st_size + 1);
	fd = open(out, O_RDONLY);
	OSMO_ASSERT(buf && fd >= 0 && read(fd, buf, st.st_size) == st.st_size);
	close(fd);
	buf[st.st_size] = '\0';
	*len = st.st_size;
	if (!strncmp(buf, "NONE the configuration was refused", 34)) {
		char line[400];
		f = fopen(err, "r");
		while (f && fgets(line, sizeof(line), f))
			printf("    reader: %s", line);
		if (f)
			fclose(f);
	}
	return buf;
}

static char *g_ref;
static size_t g_ref_len;

/* Expect cfg_text (read by r) to give exactly the reference document. */
static void same(const char *label, enum reader r, const char *cfg_text)
{
	char why[400];
	size_t len;
	char *d = build(r, cfg_text, &len);
	unsigned int line = aworker_canon_compare(g_ref, g_ref_len, d, len, why, sizeof(why));

	printf("  %s: %s\n", label, line ? why : "identical");
	CHECK(line == 0);
	talloc_free(d);
}

/* Expect a mismatch first at line exp_line, whose description contains exp_part. */
static void differs(const char *label, enum reader r, const char *cfg_text, unsigned int exp_line,
		    const char *exp_part)
{
	char why[400];
	size_t len;
	char *d = build(r, cfg_text, &len);
	unsigned int line = aworker_canon_compare(g_ref, g_ref_len, d, len, why, sizeof(why));

	printf("  %s: %s\n", label, line ? why : "identical (NOT a mismatch)");
	CHECK(line == exp_line);
	CHECK(strstr(why, exp_part) != NULL);
	talloc_free(d);
}

static void test_n(void)
{
	struct aworker_pool_cfg fc = {
		.num_workers = 1, .startup_timeout = 60, .session_echo_interval = 2, .session_echo_misses = 3,
	};
	struct aworker_cfg_model m = {};
	char *t, *d, why[300];
	size_t len;
	unsigned int n;

	printf("N1: the reference document (Supervisor's reader)\n");
	g_ref = build(SUPERVISOR, base, &g_ref_len);
	printf("%s", g_ref);
	CHECK(!strncmp(g_ref, "aworker-canonical-config 1\n", 27));

	printf("N2: the same configuration read by osmo-bsc's inline reader gives the same document\n");
	same("BSC view", BSC, base);

	printf("N3: defaults are explicit: writing a default value changes nothing\n");
	t = variant(" workers 2\n", " workers 2\n restart-delay 5\n startup-timeout 60\n session-echo-interval 2\n"
		    " session-echo-misses 3\n max-restarts 5 window 300\n on-worker-failure restart\n");
	same("all defaults written out", SUPERVISOR, t);
	same("all defaults written out (BSC)", BSC, t);
	talloc_free(t);

	printf("N4: representation, not text: comments, blank lines, extra and trailing spaces\n");
	t = variant("  remote-ip 127.0.0.2\n", "\n ! comment inside the subtree\n  remote-ip    127.0.0.2   \n\n");
	same("cs7 subtree reformatted", SUPERVISOR, t);
	talloc_free(t);
	t = variant(" worker 1 cs7-instance 1\n", " worker 1    cs7-instance   1\n");
	same("a-worker-pool line reformatted", BSC, t);
	talloc_free(t);

	printf("N5: what is not part of the document: logging, and a cs7 instance no worker owns\n");
	t = variant(" logging level set-all notice\n", " logging level set-all debug\n logging print category 1\n");
	same("log section changed", SUPERVISOR, t);
	talloc_free(t);
	t = variant(" point-code 0.23.6\n", " point-code 0.23.5\n");
	same("cs7 instance 5 changed", BSC, t);
	talloc_free(t);

	printf("N6: every difference that is part of the document is found, at its line\n");
	t = variant(" workers 2\n", " workers 2\n restart-delay 6\n");
	differs("restart-delay 6", SUPERVISOR, t, 7, "part P");
	talloc_free(t);
	t = variant(" workers 2\n", " workers 2\n session-echo-interval 5\n");
	differs("session-echo-interval 5", BSC, t, 12, "part P");
	talloc_free(t);
	t = variant(" workers 2\n", " workers 2\n supervisor-socket /run/x.sock\n");
	differs("supervisor-socket set", SUPERVISOR, t, 5, "part P");
	talloc_free(t);
	t = variant("  remote-ip 127.0.0.2\n", "  remote-ip 127.0.0.3\n");
	differs("worker 1's remote-ip", BSC, t, 27, "part C (cs7 configuration of worker 1)");
	talloc_free(t);
	t = variant("  remote-ip 127.0.0.1\n  local-ip 127.0.0.1\n", "  local-ip 127.0.0.1\n  remote-ip 127.0.0.1\n");
	differs("worker 0's commands reordered", SUPERVISOR, t, 17, "part C (cs7 configuration of worker 0)");
	talloc_free(t);
	t = variant(" point-code 0.23.4\n", " point-code 188\n");
	differs("the same point code spelled differently (a false mismatch, CD-1)", SUPERVISOR, t, 22,
		"part C (cs7 configuration of worker 1)");
	talloc_free(t);
	t = variant(" worker 0 cs7-instance 0\n worker 1 cs7-instance 1\n",
		    " worker 0 cs7-instance 1\n worker 1 cs7-instance 0\n");
	differs("workers swapped", BSC, t, 3, "part P");
	talloc_free(t);
	t = variant("  role asp\n  transport-role client\ncs7 instance 1\n",
		    "  role asp\n  transport-role client\n  no shutdown\ncs7 instance 1\n");
	differs("a command added to worker 0's asp", SUPERVISOR, t, 21, "part C (cs7 configuration of worker 0)");
	talloc_free(t);

	printf("N7: aworker_canon_compare()\n");
	n = aworker_canon_compare("a\nb\n", 4, "a\nb\n", 4, why, sizeof(why));
	printf("  identical: %u\n", n);
	CHECK(n == 0);
	n = aworker_canon_compare("a\nb\n", 4, "a\n", 2, why, sizeof(why));
	printf("  a line missing: %u, %s\n", n, why);
	CHECK(n == 2 && strstr(why, "<missing>"));
	n = aworker_canon_compare("a\nb\n", 4, "a\nb", 3, why, sizeof(why));
	printf("  no final newline: %u, %s\n", n, why);
	CHECK(n == 2);
	n = aworker_canon_compare("aworker-canonical-config 1\n", 27, "aworker-canonical-config 2\n", 27, why, sizeof(why));
	printf("  another representation version: %u, %s\n", n, why);
	CHECK(n == 1 && strstr(why, "part H"));

	printf("N8: no document for the file form\n");
	fc.cfg_file[0] = "/etc/w0.cfg";
	d = aworker_canon_build(g_ctx, &fc, &m, &len, why, sizeof(why));
	printf("  file form: %s (%s)\n", d ? "a document (WRONG)" : "none", why);
	CHECK(!d && strstr(why, "inline form"));
	t = variant(NULL, NULL);
	d = build(BSC, "log stderr\n logging level set-all notice\n", &len);
	printf("  no pool: %s\n", d);
	CHECK(!strncmp(d, "NONE ", 5));
	talloc_free(d);
	talloc_free(t);
}

int main(int argc, char **argv)
{
	char cmd[300];

	g_ctx = talloc_named_const(NULL, 0, "aworker_canon_test");
	snprintf(g_dir, sizeof(g_dir), "%s/aworker_canon.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));
	setvbuf(stdout, NULL, _IOLBF, 0);

	test_n();

	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);
	if (system(cmd) != 0)
		printf("cannot remove the test directory\n");
	printf(failures ? "FAILED\n" : "PASSED\n");
	return failures ? 1 : 0;
}

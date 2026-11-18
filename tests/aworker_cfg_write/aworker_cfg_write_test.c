/* A-worker pool: writing the configuration is refused once osmo-bsc.cfg was parsed as a view (inline form) */
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

/* With the inline form, osmo-bsc parses only its view of osmo-bsc.cfg: the worker-owned asp / as / listen nodes are not
 * in its running configuration. Any file written from it lacks them; written over osmo-bsc.cfg, it deletes them. So
 * every way of writing the configuration is refused:
 *  - VTY 'write file [PATH]', 'write', 'write memory', 'copy running-config startup-config': the real libosmovty
 *    commands, asked through vty_app_info.config_is_consistent exactly as osmo-bsc sets it;
 *  - CTRL 'SET write-config-file PATH|overwrite': osmo-bsc's real CTRL command (bsc_ctrl.c, linked from libbsc).
 * Without the inline form (no pool; file form) the same commands write as before.
 *
 * Each case runs in its own child process: the VTY's config file and the "parsed as a view" state are per process,
 * as in osmo-bsc. Nothing here opens a socket (the CTRL commands are executed from strings, without a CTRL
 * interface; no view parsed here contains an asp, as or listen node). */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/ctrl/control_cmd.h>
#include <osmocom/ctrl/control_if.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/ctrl.h>

struct gsm_network;
int bsc_ctrl_cmds_install(struct gsm_network *net);

extern void *tall_bsc_ctx;	/* defined in libbsc */
static void *ctx;
static int failures;
static char g_dir[256];

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

/* As osmo-bsc's vty_info (osmo_bsc_main.c): the same config_is_consistent. */
static struct vty_app_info vty_info = {
	.name = "aworker_cfg_write_test",
	.go_parent_cb = osmo_ss7_vty_go_parent,
	.config_is_consistent = aworker_pool_vty_config_is_consistent,
};

static const char *path_of(const char *name)
{
	static char p[4][512];
	static unsigned int n;
	char *s = p[n++ % ARRAY_SIZE(p)];

	snprintf(s, sizeof(p[0]), "%s/%s", g_dir, name);
	return s;
}

static const char *put(const char *name, const char *text)
{
	const char *path = path_of(name);
	FILE *f = fopen(path, "w");

	OSMO_ASSERT(f);
	OSMO_ASSERT(fputs(text, f) >= 0);
	OSMO_ASSERT(fclose(f) == 0);
	return path;
}

static bool exists(const char *name)
{
	struct stat st;
	return stat(path_of(name), &st) == 0;
}

/* The whole content of a file, or "" if it cannot be read. */
static const char *slurp(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n = 0;

	if (f) {
		n = fread(buf, 1, len - 1, f);
		fclose(f);
	}
	buf[n] = '\0';
	return buf;
}

/* Execute one VTY command on the ENABLE node, as a VTY session would. \returns the command's result; out gets what
 * it printed. */
static int vty_cmd(const char *cmdstr, char *out, size_t outlen)
{
	const char *capture = path_of("vty-output");
	struct vty *vty;
	vector vline;
	int fd, rc;

	fd = open(capture, O_RDWR | O_CREAT | O_TRUNC, 0600);
	OSMO_ASSERT(fd >= 0);
	vty = vty_new();
	OSMO_ASSERT(vty);
	vty->type = VTY_FILE;
	vty->fd = dup(fd);
	vty->node = ENABLE_NODE;
	vline = cmd_make_strvec(cmdstr);
	OSMO_ASSERT(vline);
	rc = cmd_execute_command(vline, vty, NULL, 0);
	cmd_free_strvec(vline);
	vty_close(vty);	/* flushes the output to the file */
	close(fd);
	slurp(capture, out, outlen);
	return rc;
}

/* Execute one CTRL command of osmo-bsc (bsc_ctrl.c). \returns its reply type; *reply its reply text. */
static enum ctrl_type ctrl_cmd(struct ctrl_handle *ch, const char *cmdstr, char *reply, size_t len)
{
	struct ctrl_cmd *cmd = ctrl_cmd_exec_from_string(ch, cmdstr);
	enum ctrl_type type;

	OSMO_ASSERT(cmd);
	type = cmd->type;
	osmo_strlcpy(reply, cmd->reply ? cmd->reply : "", len);
	talloc_free(cmd);
	return type;
}

static struct ctrl_handle *ctrl_setup(void)
{
	struct ctrl_handle *ch = ctrl_handle_alloc2(ctx, NULL, NULL, _LAST_CTRL_NODE_BSC);

	OSMO_ASSERT(ch);
	OSMO_ASSERT(bsc_ctrl_cmds_install(NULL) == 0);
	return ch;
}

/* Run fn in a child process and wait for it. */
static void in_child(const char *what, void (*fn)(void))
{
	pid_t pid;
	int status;

	printf("%s\n", what);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	OSMO_ASSERT(pid >= 0);
	if (pid == 0) {
		failures = 0;
		fn();
		fflush(stdout);
		_exit(failures ? 1 : 0);
	}
	OSMO_ASSERT(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* An inline-form osmo-bsc.cfg: the asp / as of cs7 instance 0 belong to worker 0. */
static const char *cfg_inline =
	"! osmo-bsc.cfg with the inline A-worker form\n"
	"log stderr\n"
	" logging level set-all notice\n"
	"cs7 instance 0\n"
	" point-code 0.23.3\n"
	" sccp-address msc0\n"
	"  routing-indicator PC\n"
	"  point-code 0.23.1\n"
	" asp asp-a0 2905 0 m3ua\n"
	"  remote-ip 127.0.0.1\n"
	"  role asp\n"
	" as as-a0 m3ua\n"
	"  asp asp-a0\n"
	"  routing-key 3 0.23.3\n"
	"a-worker-pool\n"
	" workers 1\n"
	" worker 0 cs7-instance 0\n";

/* case_inline(): the main configuration file it reads. */
static const char *g_inline_orig;

static void expect_vty_refused(const char *cmdstr, const char *label)
{
	char out[2048];
	int rc = vty_cmd(cmdstr, out, sizeof(out));
	bool says_why = strstr(out, "% Writing the configuration is refused: '") != NULL
			&& strstr(out, "Edit '") != NULL;

	printf("  VTY '%s': %s%s\n", label, rc == CMD_WARNING ? "refused" : "NOT refused",
	       says_why ? ", saying why" : ", WITHOUT the reason");
	CHECK(rc == CMD_WARNING);
	CHECK(says_why);
	/* The config file recorded at startup (D-8) stays the one read: a refused 'write file PATH' does not move it. */
	CHECK(host_config_file() && !strcmp(host_config_file(), g_inline_orig));
}

static void case_inline(void)
{
	char before[4096], after[4096], out[4096], reply[1024];
	char orig[512];
	struct ctrl_handle *ch;
	char cmdstr[600];

	osmo_strlcpy(orig, put("osmo-bsc.cfg", cfg_inline), sizeof(orig));	/* put() returns a reused buffer */
	g_inline_orig = orig;

	slurp(orig, before, sizeof(before));
	CHECK(aworker_pool_read_config(orig) == 0);
	printf("  parsed as the BSC view; VTY config file: %s; refusal: %s\n", host_config_file() ? (!strcmp(
	       host_config_file(), orig) ? "the file read" : "OTHER") : "none", aworker_pool_config_write_refusal() ? "yes" : "no");
	CHECK(host_config_file() && !strcmp(host_config_file(), orig));
	CHECK(aworker_pool_config_write_refusal() != NULL);

	/* Every VTY form; PATH both elsewhere and the original file itself. */
	snprintf(cmdstr, sizeof(cmdstr), "write file %s", path_of("elsewhere.cfg"));
	expect_vty_refused(cmdstr, "write file <other path>");
	snprintf(cmdstr, sizeof(cmdstr), "write file %s", orig);
	expect_vty_refused(cmdstr, "write file <osmo-bsc.cfg>");
	expect_vty_refused("write file", "write file");
	expect_vty_refused("write", "write");
	expect_vty_refused("write memory", "write memory");
	expect_vty_refused("copy running-config startup-config", "copy running-config startup-config");

	/* Showing the running configuration is not writing it. */
	CHECK(vty_cmd("write terminal", out, sizeof(out)) == CMD_SUCCESS);
	printf("  VTY 'write terminal': shown (worker declaration %s, worker asp %s)\n",
	       strstr(out, " worker 0 cs7-instance 0\n") ? "included" : "MISSING",
	       strstr(out, "asp-a0") ? "PRESENT" : "absent: not in the BSC view");
	CHECK(strstr(out, " worker 0 cs7-instance 0\n") != NULL);
	CHECK(strstr(out, "asp-a0") == NULL);

	/* CTRL: 'overwrite' (the recorded config file: this must not reach the writer) and a PATH. */
	ch = ctrl_setup();
	CHECK(ctrl_cmd(ch, "SET 1 write-config-file overwrite", reply, sizeof(reply)) == CTRL_TYPE_ERROR);
	printf("  CTRL 'SET write-config-file overwrite': %s\n",
	       strstr(reply, "Writing the configuration is refused") ? "refused, saying why" : reply);
	CHECK(strstr(reply, "Writing the configuration is refused") != NULL);
	snprintf(cmdstr, sizeof(cmdstr), "SET 2 write-config-file %s", path_of("ctrl-elsewhere.cfg"));
	CHECK(ctrl_cmd(ch, cmdstr, reply, sizeof(reply)) == CTRL_TYPE_ERROR);
	printf("  CTRL 'SET write-config-file <other path>': %s\n",
	       strstr(reply, "Writing the configuration is refused") ? "refused, saying why" : reply);
	CHECK(strstr(reply, "Writing the configuration is refused") != NULL);
	snprintf(cmdstr, sizeof(cmdstr), "SET 3 write-config-file %s", orig);
	CHECK(ctrl_cmd(ch, cmdstr, reply, sizeof(reply)) == CTRL_TYPE_ERROR);
	printf("  CTRL 'SET write-config-file <osmo-bsc.cfg>': %s\n",
	       strstr(reply, "Writing the configuration is refused") ? "refused, saying why" : reply);
	CHECK(strstr(reply, "Writing the configuration is refused") != NULL);
	CHECK(host_config_file() && !strcmp(host_config_file(), orig));

	/* Nothing was written anywhere. */
	slurp(orig, after, sizeof(after));
	printf("  osmo-bsc.cfg unchanged: %s; no .sav: %s; no other file written: %s\n",
	       !strcmp(before, after) ? "yes" : "NO", !exists("osmo-bsc.cfg.sav") ? "yes" : "NO",
	       !exists("elsewhere.cfg") && !exists("ctrl-elsewhere.cfg") ? "yes" : "NO");
	CHECK(!strcmp(before, after));
	CHECK(!exists("osmo-bsc.cfg.sav"));
	CHECK(!exists("elsewhere.cfg"));
	CHECK(!exists("ctrl-elsewhere.cfg"));
}

/* An inline file whose BSC view fails to parse: the refusal holds from the moment the view is chosen. */
static void case_inline_parse_error(void)
{
	const char *orig = put("broken.cfg",
			       "no-such-command\n"
			       "cs7 instance 0\n"
			       " point-code 0.23.3\n"
			       " asp asp-a0 2905 0 m3ua\n"
			       "  remote-ip 127.0.0.1\n"
			       "a-worker-pool\n"
			       " workers 1\n"
			       " worker 0 cs7-instance 0\n");

	CHECK(aworker_pool_read_config(orig) != 0);
	printf("  parse refused; refusal: %s\n", aworker_pool_config_write_refusal() ? "yes" : "no");
	CHECK(aworker_pool_config_write_refusal() != NULL);
}

/* Without the inline form, writing works exactly as before. */
static void expect_written(const char *orig, const char *what)
{
	char out[4096], reply[1024], cmdstr[600];
	struct ctrl_handle *ch;
	int rc;

	CHECK(aworker_pool_read_config(orig) == 0);
	printf("  %s: VTY config file: %s; refusal: %s\n", what,
	       host_config_file() && !strcmp(host_config_file(), orig) ? "the file read" : "OTHER",
	       aworker_pool_config_write_refusal() ? "yes" : "no");
	CHECK(host_config_file() && !strcmp(host_config_file(), orig));
	CHECK(aworker_pool_config_write_refusal() == NULL);

	rc = vty_cmd("write", out, sizeof(out));
	printf("  VTY 'write': %s\n", rc == CMD_SUCCESS && strstr(out, "Configuration saved to ") ? "saved" : out);
	CHECK(rc == CMD_SUCCESS);
	CHECK(exists("main.cfg.sav"));

	snprintf(cmdstr, sizeof(cmdstr), "write file %s", path_of("elsewhere.cfg"));
	rc = vty_cmd(cmdstr, out, sizeof(out));
	printf("  VTY 'write file <other path>': %s\n", rc == CMD_SUCCESS && exists("elsewhere.cfg") ? "saved" : out);
	CHECK(rc == CMD_SUCCESS);
	CHECK(exists("elsewhere.cfg"));

	ch = ctrl_setup();
	snprintf(cmdstr, sizeof(cmdstr), "SET 1 write-config-file %s", path_of("ctrl-elsewhere.cfg"));
	CHECK(ctrl_cmd(ch, cmdstr, reply, sizeof(reply)) == CTRL_TYPE_SET_REPLY);
	printf("  CTRL 'SET write-config-file <other path>': %s\n", exists("ctrl-elsewhere.cfg") ? reply : "NOT saved");
	CHECK(exists("ctrl-elsewhere.cfg"));
	CHECK(ctrl_cmd(ch, "SET 2 write-config-file overwrite", reply, sizeof(reply)) == CTRL_TYPE_SET_REPLY);
	printf("  CTRL 'SET write-config-file overwrite': %s\n", reply);
}

static void case_no_pool(void)
{
	unlink(path_of("main.cfg.sav"));
	unlink(path_of("elsewhere.cfg"));
	unlink(path_of("ctrl-elsewhere.cfg"));
	expect_written(put("main.cfg",
			   "log stderr\n"
			   " logging level set-all notice\n"
			   "cs7 instance 0\n"
			   " point-code 0.23.3\n"), "no pool");
}

static void pool_section(const char *name, const char *what);

static void case_file_form(void)
{
	unlink(path_of("main.cfg.sav"));
	unlink(path_of("elsewhere.cfg"));
	unlink(path_of("ctrl-elsewhere.cfg"));
	expect_written(put("main.cfg",
			   "log stderr\n"
			   " logging level set-all notice\n"
			   "cs7 instance 0\n"
			   " point-code 0.23.3\n"
			   "a-worker-pool\n"
			   " workers 1\n"
			   " worker 0 config-file /nonexistent/worker0.cfg\n"), "file form");
	pool_section("elsewhere.cfg", "the keys of S2-5 at their defaults");
}

/* What 'write file' wrote of the 'a-worker-pool' section: S2-5's keys only when they are set (so files and
 * 'show running-config' of existing configurations are unchanged). */
static void pool_section(const char *name, const char *what)
{
	char buf[4096], *p;
	size_t n;
	FILE *f = fopen(path_of(name), "r");

	OSMO_ASSERT(f);
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	p = strstr(buf, "a-worker-pool\n");
	CHECK(p != NULL);
	if (!p)
		return;
	*(strstr(p, "\n!") ? strstr(p, "\n!") + 1 : p + strlen(p)) = '\0';
	printf("  written, %s:\n%s", what, p);
}

static void case_session_keys(void)
{
	unlink(path_of("main.cfg.sav"));
	unlink(path_of("elsewhere.cfg"));
	unlink(path_of("ctrl-elsewhere.cfg"));
	expect_written(put("main.cfg",
			   "log stderr\n"
			   " logging level set-all notice\n"
			   "a-worker-pool\n"
			   " workers 1\n"
			   " worker 0 config-file /nonexistent/worker0.cfg\n"
			   " supervisor-socket /run/osmocom/bsc-aworker.sock\n"
			   " session-echo-interval 7\n"
			   " session-echo-misses 9\n"), "file form with the S2-5 keys set");
	pool_section("elsewhere.cfg", "the keys of S2-5 set");
}

/* No worker at all, only the session socket: still a setting of the section, so it is written. */
static void case_socket_only(void)
{
	unlink(path_of("main.cfg.sav"));
	unlink(path_of("elsewhere.cfg"));
	unlink(path_of("ctrl-elsewhere.cfg"));
	expect_written(put("main.cfg",
			   "log stderr\n"
			   " logging level set-all notice\n"
			   "a-worker-pool\n"
			   " supervisor-socket /run/osmocom/bsc-aworker.sock\n"), "no workers, only supervisor-socket");
	pool_section("elsewhere.cfg", "only supervisor-socket set");
}

int main(int argc, char **argv)
{
	char cmd[300];

	ctx = talloc_named_const(NULL, 0, "aworker_cfg_write_test");
	tall_bsc_ctx = ctx;
	msgb_talloc_ctx_init(ctx, 0);
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

	snprintf(g_dir, sizeof(g_dir), "%s/aworker_cfg_write.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	OSMO_ASSERT(mkdtemp(g_dir));

	in_child("inline form: every write refused", case_inline);
	in_child("inline form, BSC view does not parse: refused all the same", case_inline_parse_error);
	in_child("no pool: writes as before", case_no_pool);
	in_child("file form: writes as before", case_file_form);
	in_child("file form with the S2-5 keys set: they are written back", case_session_keys);
	in_child("only supervisor-socket: the section is written", case_socket_only);

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

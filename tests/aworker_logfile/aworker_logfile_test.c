/* D-S10 regression test (S2-3): several processes appending to one log file through libosmocore's file target */
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

/* Decision B2 lets the Supervisor and osmo-bsc append to the same configured 'log file'. This test keeps the evidence
 * for that: NPROC processes each open the same file with log_target_create_file() -- what 'log file FILE' does --
 * and write NLINES lines of 10 to 4000 octets at the same time; afterwards every line must be whole and present
 * exactly once. Both modes of 'log file FILE [nonblocking-io|blocking-io]' are tested (in libosmocore 1.14 both are a
 * stdio stream opened "a", one fputs() + fflush() per message). NOT tested, so no claim is made for them: the 'wq'
 * mode, other filesystems than the one $TMPDIR (or /tmp) is on, longer lines, log rotation, a full disk. */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

#define NPROC	4
#define NLINES	2500
#define MAXLEN	4000

static const struct log_info_cat cats[] = { [0] = { .name = "DT", .enabled = 1, .loglevel = LOGL_DEBUG } };
static const struct log_info info = { .cat = cats, .num_cat = ARRAY_SIZE(cats) };

/* All writers start together: each waits until the parent closes the write end of this pipe. */
static int g_start[2];

static void writer(const char *path, int p, bool blocking)
{
	void *ctx = talloc_named_const(NULL, 0, "writer");
	static char payload[MAXLEN + 1];
	struct log_target *t;
	unsigned int seed = p + 1;
	int i;

	osmo_init_logging2(ctx, &info);
	log_target_destroy(osmo_stderr_target);
	t = log_target_create_file(path);
	if (!t)
		_exit(2);
	log_target_file_set_nonblock(t, blocking ? 0 : 1);
	log_set_print_timestamp(t, 0);
	log_set_print_category(t, 0);
	log_set_print_category_hex(t, 0);
	log_set_print_level(t, 0);
	log_set_print_filename2(t, LOG_FILENAME_NONE);
	log_set_all_filter(t, 1);
	log_set_category_filter(t, 0, 1, LOGL_DEBUG);
	log_add_target(t);
	{
		char c;
		close(g_start[1]);
		if (read(g_start[0], &c, 1) < 0) {}	/* EOF once every writer is set up */
	}
	for (i = 0; i < NLINES; i++) {
		int len = 10 + rand_r(&seed) % (MAXLEN - 9);
		memset(payload, 'a' + p, len);
		payload[len] = '\0';
		LOGP(0, LOGL_NOTICE, "P%d:%d:%d:%s\n", p, i, len, payload);
	}
	_exit(0);
}

/* \returns the number of malformed lines; *complete: every line of every process present exactly once; *switches: how
 * often two consecutive lines come from different writers (the writers really ran at the same time). */
static unsigned int verify(const char *path, bool *complete, unsigned int *total, unsigned int *switches)
{
	int last = -1;
	static unsigned char seen[NPROC][NLINES];
	static char line[MAXLEN + 128];
	unsigned int bad = 0, p, i;
	FILE *f = fopen(path, "r");

	memset(seen, 0, sizeof(seen));
	*total = 0;
	*switches = 0;
	if (!f)
		return 1;
	while (fgets(line, sizeof(line), f)) {
		int pp, ii, len, off = 0;
		size_t l = strlen(line);
		(*total)++;
		if (l == 0 || line[l - 1] != '\n' || sscanf(line, "P%d:%d:%d:%n", &pp, &ii, &len, &off) != 3 || !off
		    || pp < 0 || pp >= NPROC || ii < 0 || ii >= NLINES || len < 10 || len > MAXLEN
		    || l - 1 != (size_t)off + len || strspn(line + off, (char[]){ 'a' + pp, 0 }) != (size_t)len) {
			bad++;
			continue;
		}
		seen[pp][ii]++;
		*switches += last >= 0 && last != pp;
		last = pp;
	}
	fclose(f);
	*complete = true;
	for (p = 0; p < NPROC; p++)
		for (i = 0; i < NLINES; i++)
			*complete &= seen[p][i] == 1;
	return bad;
}

int main(int argc, char **argv)
{
	static const struct { const char *name; bool blocking; } modes[] = {
		{ "default (nonblocking-io)", false },
		{ "blocking-io", true },
	};
	char dir[256], path[300];
	unsigned int m, failures = 0;

	snprintf(dir, sizeof(dir), "%s/aworker_logfile.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
	if (!mkdtemp(dir))
		return 1;
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("%d processes appending %d lines each (10 to %d octets) to one 'log file' at the same time\n", NPROC, NLINES,
	       MAXLEN);
	for (m = 0; m < ARRAY_SIZE(modes); m++) {
		unsigned int bad, total, switches;
		bool complete;
		int p, status, ok_children = 0;

		snprintf(path, sizeof(path), "%s/shared.log", dir);
		unlink(path);
		fflush(stdout);
		if (pipe(g_start))
			return 1;
		for (p = 0; p < NPROC; p++) {
			pid_t pid = fork();
			if (pid < 0)
				return 1;
			if (pid == 0)
				writer(path, p, modes[m].blocking);
		}
		close(g_start[0]);
		close(g_start[1]);	/* go */
		while (wait(&status) > 0)
			ok_children += WIFEXITED(status) && WEXITSTATUS(status) == 0;
		bad = verify(path, &complete, &total, &switches);
		fprintf(stderr, "%s: %u switches between writers\n", modes[m].name, switches);
		/* Without real interleaving the result proves nothing: say so, and fail. */
		if (switches < NPROC * 10) {
			printf("  %s: the writers did not run at the same time (%u switches): inconclusive\n", modes[m].name,
			       switches);
			failures++;
		}
		printf("  %s: writers %s; %s; %s\n", modes[m].name, ok_children == NPROC ? "all ended normally" : "FAILED",
		       bad ? "SOME LINES MALFORMED" : "no malformed line",
		       complete && total == NPROC * NLINES ? "every line present exactly once" : "LINES MISSING OR DOUBLED");
		if (ok_children != NPROC || bad || !complete || total != NPROC * NLINES) {
			printf("    (%u lines, %u malformed)\n", total, bad);
			failures++;
		}
		unlink(path);
	}
	printf("  wq: not tested; no claim\n");
	rmdir(dir);
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

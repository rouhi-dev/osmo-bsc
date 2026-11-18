/* A-worker: the canonical configuration document (decision CD-1, D-S5; see aworker_canon.h) */
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

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/vector.h>

#include <osmocom/bsc/aworker_canon.h>
#include <osmocom/bsc/aworker_cfg_view.h>
#include <osmocom/bsc/aworker_pool.h>

struct doc {
	char *buf;
	size_t len;
	bool err;
	char *why;
	size_t why_len;
};

static void doc_fail(struct doc *d, const char *what)
{
	if (!d->err && d->why_len)
		snprintf(d->why, d->why_len, "%s", what);
	d->err = true;
}

static void add(struct doc *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(struct doc *d, const char *fmt, ...)
{
	va_list ap;
	char *n;

	if (d->err)
		return;
	va_start(ap, fmt);
	n = talloc_vasprintf_append_buffer(d->buf, fmt, ap);
	va_end(ap);
	if (!n) {
		doc_fail(d, "out of memory");
		return;
	}
	d->buf = n;
	d->len = strlen(n);
	if (d->len > AWORKER_CANON_MAX)
		doc_fail(d, "the canonical document would be larger than its limit");
}

/* One command line of a C part: "C <worker> <depth> <tokens>". */
static void add_cmd(struct doc *d, unsigned int worker, const struct aworker_cfg_line *l, unsigned int lineno)
{
	vector v = NULL;
	unsigned int i;
	bool first = true;
	char why[160];

	if (l->overlong) {
		snprintf(why, sizeof(why), "line %u is longer than the VTY reads in one piece", lineno);
		doc_fail(d, why);
		return;
	}
	/* The parser's own tokenizer, so that what is compared is what it reads. No indentation (the parser has
	 * checked that; depth comes from the model). */
	if (cmd_make_strvec2(l->text, NULL, &v) != CMD_SUCCESS || !v) {
		if (v)
			cmd_free_strvec(v);
		snprintf(why, sizeof(why), "line %u cannot be tokenized", lineno);
		doc_fail(d, why);
		return;
	}
	add(d, "C %u %u", worker, l->depth);
	for (i = 0; i < vector_active(v); i++) {
		const char *tok = vector_slot(v, i);
		if (!tok)
			continue;
		add(d, " %s", tok);
		first = false;
	}
	add(d, "\n");
	cmd_free_strvec(v);
	if (first)
		doc_fail(d, "a command line without tokens");
}

char *aworker_canon_build(void *ctx, const struct aworker_pool_cfg *cfg, const struct aworker_cfg_model *m, size_t *len,
			  char *why, size_t why_len)
{
	struct doc d = { .why = why, .why_len = why_len };
	unsigned int i, j;

	if (why_len)
		why[0] = '\0';
	d.buf = talloc_strdup(ctx, "");
	if (!d.buf) {
		doc_fail(&d, "out of memory");
		return NULL;
	}

	/* H */
	add(&d, "%s %u\n", AWORKER_CANON_HEADER, AWORKER_CANON_VERSION);

	/* P: effective values, defaults explicit, fixed order. */
	add(&d, "P workers %u\n", cfg->num_workers);
	for (i = 0; i < cfg->num_workers && i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (cfg->cfg_file[i] || !cfg->has_cs7_instance[i]) {
			char w[96];
			snprintf(w, sizeof(w), "worker %u does not use the inline form ('worker %u cs7-instance <N>')", i, i);
			doc_fail(&d, w);
			break;
		}
		add(&d, "P worker %u cs7-instance %u\n", i, cfg->cs7_instance[i]);
	}
	add(&d, "P supervisor-socket %s\n", cfg->supervisor_socket ? cfg->supervisor_socket : "none");
	add(&d, "P startup-timeout %u\n", cfg->startup_timeout);
	add(&d, "P restart-delay %u\n", cfg->restart_delay);
	add(&d, "P restart-delay-max %u\n", cfg->restart_delay_max);
	add(&d, "P stable-time %u\n", cfg->stable_time);
	add(&d, "P max-restarts %u window %u\n", cfg->max_restarts, cfg->restart_window);
	add(&d, "P on-worker-failure %s\n", get_value_string(aworker_failure_policy_names, cfg->on_failure));
	add(&d, "P session-echo-interval %u\n", cfg->session_echo_interval);
	add(&d, "P session-echo-misses %u\n", cfg->session_echo_misses);

	/* C: each worker's cs7 subtree(s), in file order. */
	for (i = 0; i < cfg->num_workers && i < AWORKER_POOL_MAX_WORKERS && !d.err; i++) {
		for (j = 0; j < m->num_lines; j++) {
			const struct aworker_cfg_line *l = &m->line[j];
			if (!l->command || l->sec != AWORKER_CFG_SEC_CS7 || l->cs7_id != (int)cfg->cs7_instance[i])
				continue;
			add_cmd(&d, i, l, j + 1);
		}
	}

	if (d.err) {
		talloc_free(d.buf);
		return NULL;
	}
	*len = d.len;
	return d.buf;
}

/* The line starting at *pos (up to, not including, '\n'), or NULL at the end; advances *pos past it. */
static const char *next_line(const char *buf, size_t blen, size_t *pos, size_t *llen)
{
	const char *s, *nl;

	if (*pos >= blen)
		return NULL;
	s = buf + *pos;
	nl = memchr(s, '\n', blen - *pos);
	*llen = nl ? (size_t)(nl - s) : blen - *pos;
	*pos += *llen + (nl ? 1 : 0);
	return s;
}

/* Where a line belongs, as a rank in document order: H 0, P 1, C 2 + worker. */
static unsigned int line_rank(const char *l, size_t len)
{
	unsigned int w = 0;

	if (len > 2 && l[0] == 'C' && l[1] == ' ') {
		for (l += 2, len -= 2; len && *l >= '0' && *l <= '9' && w < 1000; l++, len--)
			w = w * 10 + (*l - '0');
		return 2 + w;
	}
	if (len > 2 && l[0] == 'P' && l[1] == ' ')
		return 1;
	return 0;
}

static void describe(char *why, size_t why_len, unsigned int lineno, const char *a, size_t alen, const char *b,
		     size_t blen)
{
	unsigned int rank;
	char part[48];

	if (!why_len)
		return;
	/* The documents diverge in the earlier of the two lines' parts: a line inserted on one side is the one that
	 * belongs there. */
	if (a && b)
		rank = OSMO_MIN(line_rank(a, alen), line_rank(b, blen));
	else
		rank = a ? line_rank(a, alen) : line_rank(b, blen);
	if (rank >= 2)
		snprintf(part, sizeof(part), "part C (cs7 configuration of worker %u)", rank - 2);
	else if (rank == 1)
		snprintf(part, sizeof(part), "part P (a-worker-pool)");
	else
		snprintf(part, sizeof(part), "part H (representation version)");
	snprintf(why, why_len, "line %u, %s: ours '%.*s', theirs '%.*s'", lineno, part,
		 a ? (int)OSMO_MIN(alen, 200) : 9, a ? a : "<missing>", b ? (int)OSMO_MIN(blen, 200) : 9,
		 b ? b : "<missing>");
}

unsigned int aworker_canon_compare(const char *ours, size_t ours_len, const char *theirs, size_t theirs_len,
				   char *why, size_t why_len)
{
	size_t pa = 0, pb = 0, la = 0, lb = 0;
	unsigned int lineno = 0;

	if (why_len)
		why[0] = '\0';
	while (1) {
		const char *a = next_line(ours, ours_len, &pa, &la);
		const char *b = next_line(theirs, theirs_len, &pb, &lb);

		lineno++;
		if (!a && !b)
			break;
		if (!a || !b || la != lb || memcmp(a, b, la)) {
			describe(why, why_len, lineno, a, la, b, lb);
			return lineno;
		}
	}
	/* Same lines; the bytes may still differ in a final newline. Exact comparison means exactly that. */
	if (ours_len != theirs_len || memcmp(ours, theirs, ours_len)) {
		if (why_len)
			snprintf(why, why_len, "line %u: the documents differ in their final newline", lineno - 1);
		return lineno > 1 ? lineno - 1 : 1;
	}
	return 0;
}

/* A-worker pool: process-specific views of osmo-bsc.cfg (see aworker_cfg_view.h) */
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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/vector.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_cfg_view.h>

#define MAX_DEPTH	32

/* libosmovty's indent_cmp() (command.c), which is static there: 0 equal, < 0 a is a proper prefix of b, > 0 b is a
 * proper prefix of a, EINVAL if neither is a prefix of the other. */
static int indent_cmp(const char *a, const char *b)
{
	size_t al = strlen(a), bl = strlen(b);

	if (al > bl) {
		if (bl && strncmp(a, b, bl) != 0)
			return EINVAL;
		return 1;
	}
	if (al && strncmp(a, b, al) != 0)
		return EINVAL;
	return (al < bl) ? -1 : 0;
}

static const char *tok(vector v, unsigned int i)
{
	if (!v || i >= vector_active(v))
		return NULL;
	return vector_slot(v, i);
}

static bool tok_is(vector v, unsigned int i, const char *want)
{
	const char *t = tok(v, i);
	return t && !strcmp(t, want);
}

/* A decimal number without sign or trailing characters, as <0-N> ranges accept; -1 otherwise. */
static long tok_num(vector v, unsigned int i)
{
	const char *t = tok(v, i);
	char *end;
	long n;

	if (!t || !*t || !isdigit((unsigned char)*t))
		return -1;
	errno = 0;
	n = strtol(t, &end, 10);
	if (errno || *end)
		return -1;
	return n;
}

static uint8_t proto_of(vector v, unsigned int i)
{
	const char *t = tok(v, i);

	if (!t)
		return OSMO_SS7_ASP_PROT_NONE;
	if (!strcmp(t, "m3ua"))
		return OSMO_SS7_ASP_PROT_M3UA;
	if (!strcmp(t, "sua"))
		return OSMO_SS7_ASP_PROT_SUA;
	if (!strcmp(t, "ipa"))
		return OSMO_SS7_ASP_PROT_IPA;
	return OSMO_SS7_ASP_PROT_NONE;
}

/* asp / as / listen, or their 'no' forms, directly below a cs7 instance: the commands that create a transport.
 * \returns the keyword, or NULL. *proto is the ASP protocol the command names, if any. */
static const char *transport_cmd(vector v, uint8_t *proto)
{
	unsigned int o = tok_is(v, 0, "no") ? 1 : 0;

	*proto = OSMO_SS7_ASP_PROT_NONE;
	if (tok_is(v, o, "asp")) {
		if (!o)
			*proto = proto_of(v, 4);	/* asp NAME <rport> <lport> PROTO [(sctp|tcp)] */
		return "asp";
	}
	if (tok_is(v, o, "as")) {
		if (!o)
			*proto = proto_of(v, 2);	/* as NAME PROTO */
		return "as";
	}
	if (tok_is(v, o, "listen")) {
		*proto = proto_of(v, o + 1);	/* [no] listen PROTO <port> [...] */
		return "listen";
	}
	return NULL;
}

static void note_transport(struct aworker_cfg_cs7 *c, const char *kw, unsigned int lineno, uint8_t proto)
{
	if (!c->num_transport++) {
		osmo_strlcpy(c->transport, kw, sizeof(c->transport));
		c->transport_lineno = lineno;
	}
	if (proto == OSMO_SS7_ASP_PROT_NONE)
		return;
	if (c->proto == OSMO_SS7_ASP_PROT_NONE)
		c->proto = proto;
	else if (c->proto != proto)
		c->proto_conflict = true;
}

/* A command of the 'a-worker-pool' node: record workers and the per-worker declarations. */
static void note_pool_cmd(struct aworker_cfg_model *m, vector v, unsigned int lineno)
{
	long idx;

	if (tok_is(v, 0, "workers")) {
		long n = tok_num(v, 1);
		if (n >= 0)
			m->num_workers = (unsigned int)n;
		return;
	}
	if (tok_is(v, 0, "worker")) {
		idx = tok_num(v, 1);
		if (idx < 0 || idx >= AWORKER_CFG_VIEW_MAX_WORKERS)
			return;	/* the parser refuses it */
		if (tok_is(v, 2, "config-file") && tok(v, 3)) {
			m->decl[idx].file = true;
			m->decl[idx].file_lineno = lineno;
		} else if (tok_is(v, 2, "cs7-instance")) {
			long x = tok_num(v, 3);
			if (x < 0 || x >= AWORKER_CFG_VIEW_MAX_CS7)
				return;	/* the parser refuses it */
			m->decl[idx].inl = true;
			m->decl[idx].inl_lineno = lineno;
			m->decl[idx].cs7_id = (uint32_t)x;
		}
		return;
	}
	if (tok_is(v, 0, "no") && tok_is(v, 1, "worker")) {
		idx = tok_num(v, 2);
		if (idx < 0 || idx >= AWORKER_CFG_VIEW_MAX_WORKERS)
			return;
		if (tok_is(v, 3, "config-file"))
			m->decl[idx].file = false;
		else if (tok_is(v, 3, "cs7-instance"))
			m->decl[idx].inl = false;
	}
}

/* The model of the configuration read from f; file is only its name (diagnostics). */
static struct aworker_cfg_model *model_build_fp(void *ctx, const char *file, FILE *f)
{
	struct aworker_cfg_model *m;
	char *levels[MAX_DEPTH] = {};
	unsigned int num_levels = 0, alloc = 0;
	enum aworker_cfg_section sec = AWORKER_CFG_SEC_NONE;
	int cs7_id = -1;
	bool in_transport = false, in_log_file = false, in_no_log = false;
	char buf[VTY_BUFSIZ];

	m = talloc_zero(ctx, struct aworker_cfg_model);
	OSMO_ASSERT(m);
	m->file = talloc_strdup(m, file);

	/* Read exactly as config_from_file() does: one fgets() per line into a VTY_BUFSIZ buffer. */
	while (fgets(buf, sizeof(buf), f)) {
		struct aworker_cfg_line *l;
		size_t len = strlen(buf);
		char *indent = NULL;
		vector v = NULL;
		int rc;

		if (m->num_lines == alloc) {
			alloc = alloc ? alloc * 2 : 64;
			m->line = talloc_realloc(m, m->line, struct aworker_cfg_line, alloc);
			OSMO_ASSERT(m->line);
		}
		l = &m->line[m->num_lines++];
		memset(l, 0, sizeof(*l));
		l->text = talloc_strdup(m, buf);
		l->overlong = len == sizeof(buf) - 1 && buf[len - 1] != '\n';
		l->cs7_id = -1;

		rc = cmd_make_strvec2(buf, &indent, &v);
		if (rc != CMD_SUCCESS) {
			/* Whitespace other than space and tab before the first token. */
			l->bad_indent = true;
			l->sec = sec;
			l->cs7_id = cs7_id;
			l->transport = in_transport;
			l->log_file = in_log_file;
			l->no_log = in_no_log;
			talloc_free(indent);
			continue;
		}
		if (!v) {
			/* Blank or comment line: the parser skips it. It belongs to whatever came before it. */
			l->sec = sec;
			l->cs7_id = cs7_id;
			l->transport = in_transport;
			l->log_file = in_log_file;
			l->no_log = in_no_log;
			talloc_free(indent);
			continue;
		}
		l->command = true;

		/* The node depth of this line, as config_from_file() derives it from the indentation. */
		if (!num_levels) {
			levels[0] = talloc_strdup(m, indent);
			num_levels = 1;
			l->depth = 0;
		} else {
			int cmp = indent_cmp(indent, levels[num_levels - 1]);
			if (cmp == EINVAL) {
				l->bad_indent = true;
			} else if (cmp > 0) {
				if (num_levels >= MAX_DEPTH) {
					l->bad_indent = true;
				} else {
					/* Deeper than the line before: a child of that command. */
					levels[num_levels++] = talloc_strdup(m, indent);
					l->depth = num_levels - 1;
				}
			} else {
				/* Same or less indentation: it must match an open level exactly. */
				unsigned int d = num_levels - 1;
				while (cmp < 0 && d > 0) {
					d--;
					cmp = indent_cmp(indent, levels[d]);
				}
				if (cmp != 0) {
					l->bad_indent = true;
				} else {
					l->depth = d;
					num_levels = d + 1;
				}
			}
		}
		if (l->bad_indent) {
			l->sec = sec;
			l->cs7_id = cs7_id;
			l->transport = in_transport;
			l->log_file = in_log_file;
			l->no_log = in_no_log;
			cmd_free_strvec(v);
			talloc_free(indent);
			continue;
		}

		if (l->depth == 0) {
			in_transport = false;
			cs7_id = -1;
			/* D-2: a file target's section or its 'no' form (log_file); every 'no log' form (no_log). A 'no log
			 * file' line has both: keep_line() checks log_file first. */
			in_log_file = (tok_is(v, 0, "log") && tok_is(v, 1, "file"))
				      || (tok_is(v, 0, "no") && tok_is(v, 1, "log") && tok_is(v, 2, "file"));
			in_no_log = tok_is(v, 0, "no") && tok_is(v, 1, "log");
			if (tok_is(v, 0, "cs7") && tok_is(v, 1, "instance")) {
				long n = tok_num(v, 2);
				sec = AWORKER_CFG_SEC_CS7;
				if (n >= 0 && n < AWORKER_CFG_VIEW_MAX_CS7) {
					struct aworker_cfg_cs7 *c = &m->cs7[n];
					cs7_id = (int)n;
					if (!c->num_sections++)
						c->lineno = m->num_lines;
					else if (!c->dup_lineno)
						c->dup_lineno = m->num_lines;
				}
			} else if (tok_is(v, 0, "a-worker-pool")) {
				sec = AWORKER_CFG_SEC_POOL;
				if (!m->pool_lineno)
					m->pool_lineno = m->num_lines;
			} else if (tok_is(v, 0, "log")) {
				sec = AWORKER_CFG_SEC_LOG;
			} else {
				sec = AWORKER_CFG_SEC_OTHER;
			}
		} else if (l->depth == 1) {
			in_transport = false;
			if (sec == AWORKER_CFG_SEC_CS7) {
				uint8_t proto;
				const char *kw = transport_cmd(v, &proto);
				if (kw) {
					in_transport = true;
					l->transport_head = true;
					if (cs7_id >= 0)
						note_transport(&m->cs7[cs7_id], kw, m->num_lines, proto);
				}
			} else if (sec == AWORKER_CFG_SEC_POOL) {
				note_pool_cmd(m, v, m->num_lines);
			}
		}
		l->sec = sec;
		l->cs7_id = cs7_id;
		l->transport = in_transport;
		l->log_file = in_log_file;
		l->no_log = in_no_log;
		l->exit_end = tok_is(v, 0, "exit") || tok_is(v, 0, "end");

		cmd_free_strvec(v);
		talloc_free(indent);
	}
	/* The level strings are talloc children of m. */
	return m;
}

int aworker_cfg_model_build(void *ctx, const char *file, struct aworker_cfg_model **out)
{
	FILE *f;

	*out = NULL;
	f = fopen(file, "r");
	if (!f)
		return -errno;
	*out = model_build_fp(ctx, file, f);
	fclose(f);
	return 0;
}

int aworker_cfg_model_build_buf(void *ctx, const char *name, const char *buf, size_t len,
				struct aworker_cfg_model **out)
{
	FILE *f;

	*out = NULL;
	/* fmemopen() needs at least one octet; an empty configuration is read from an empty stream. */
	f = len ? fmemopen((void *)buf, len, "r") : fopen("/dev/null", "r");
	if (!f)
		return -errno;
	*out = model_build_fp(ctx, name, f);
	fclose(f);
	return 0;
}

/***********************************************************************
 * Reading the source configuration once
 ***********************************************************************/

static void (*g_read_stable_hook)(enum aworker_cfg_read_stable_stage stage, const char *path, char *buf, size_t len);

void aworker_cfg_read_stable_set_test_hook(void (*hook)(enum aworker_cfg_read_stable_stage stage, const char *path,
							char *buf, size_t len))
{
	g_read_stable_hook = hook;
}

static bool same_time(const struct timespec *a, const struct timespec *b)
{
	return a->tv_sec == b->tv_sec && a->tv_nsec == b->tv_nsec;
}

/* Read up to len octets at offset 0 .. len-1 with pread(); \returns the number read (short only at end-of-file). */
static ssize_t pread_full(int fd, char *buf, size_t len, off_t off)
{
	size_t got = 0;

	while (got < len) {
		ssize_t n = pread(fd, buf + got, len - got, off + (off_t)got);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return -1;
		if (!n)
			break;
		got += (size_t)n;
	}
	return (ssize_t)got;
}

int aworker_cfg_read_stable(void *ctx, const char *path, size_t max, struct aworker_cfg_source **out)
{
	struct aworker_cfg_source *src;
	struct stat a, b, p;
	char check[4096];
	size_t len, off;
	ssize_t n;
	int fd, rc;

	*out = NULL;
	/* Never open anything but a regular file: opening a FIFO, even briefly, would let its writer's open()
	 * succeed and its data be lost when we close it. */
	if (stat(path, &p) < 0)
		return -errno;
	if (!S_ISREG(p.st_mode))
		return -EINVAL;
	if (g_read_stable_hook)
		g_read_stable_hook(AWORKER_CFG_READ_STABLE_BEFORE_OPEN, path, NULL, 0);
	/* O_NONBLOCK: if the path is swapped for a FIFO after the stat(), this must not block; it is refused by
	 * the fstat() below. Regular files ignore the flag. */
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -errno;
	if (fstat(fd, &a) < 0) {
		rc = -errno;
		goto out_close;
	}
	if (!S_ISREG(a.st_mode)) {
		rc = -EINVAL;
		goto out_close;
	}
	if (a.st_size < 0 || (unsigned long long)a.st_size > max) {
		rc = -EFBIG;
		goto out_close;
	}
	len = (size_t)a.st_size;

	src = talloc_zero(ctx, struct aworker_cfg_source);
	OSMO_ASSERT(src);
	src->path = talloc_strdup(src, path);
	src->buf = talloc_size(src, len + 1);
	OSMO_ASSERT(src->path && src->buf);

	/* The one read. A file that ends early or goes on past its size changed while it was read. */
	n = pread_full(fd, src->buf, len, 0);
	if (n < 0) {
		rc = -errno;
		goto out_free;
	}
	if ((size_t)n != len || pread_full(fd, check, 1, (off_t)len) != 0) {
		rc = -ESTALE;
		goto out_free;
	}
	src->buf[len] = '\0';
	src->len = len;

	if (g_read_stable_hook)
		g_read_stable_hook(AWORKER_CFG_READ_STABLE_AFTER_READ, path, src->buf, len);

	/* The same file, unchanged: the open file's size and high-resolution modification and change times, and the
	 * path still naming that file (device and inode). The size checks (here and above) look redundant with the
	 * change time, which any size change updates, but they also hold where timestamps are coarse. */
	if (fstat(fd, &b) < 0 || stat(path, &p) < 0) {
		rc = -ESTALE;
		goto out_free;
	}
	if (a.st_size != b.st_size
	    || !same_time(&a.st_mtim, &b.st_mtim) || !same_time(&a.st_ctim, &b.st_ctim)
	    || p.st_dev != a.st_dev || p.st_ino != a.st_ino) {
		rc = -ESTALE;
		goto out_free;
	}

	/* A second pass over the contents, compared octet by octet with what was read. */
	for (off = 0; off < len; off += (size_t)n) {
		size_t want = OSMO_MIN(sizeof(check), len - off);
		n = pread_full(fd, check, want, (off_t)off);
		if (n != (ssize_t)want || memcmp(check, src->buf + off, want)) {
			rc = -ESTALE;
			goto out_free;
		}
	}

	close(fd);
	*out = src;
	return 0;

out_free:
	talloc_free(src);
out_close:
	close(fd);
	return rc;
}

bool aworker_cfg_model_is_inline(const struct aworker_cfg_model *m)
{
	unsigned int i;

	for (i = 0; i < m->num_workers && i < AWORKER_CFG_VIEW_MAX_WORKERS; i++) {
		if (m->decl[i].inl)
			return true;
	}
	return false;
}

int aworker_cfg_model_owner(const struct aworker_cfg_model *m, uint32_t cs7_id)
{
	unsigned int i;

	for (i = 0; i < m->num_workers && i < AWORKER_CFG_VIEW_MAX_WORKERS; i++) {
		if (m->decl[i].inl && m->decl[i].cs7_id == cs7_id)
			return (int)i;
	}
	return -1;
}

static void problem(struct aworker_cfg_problem *out, unsigned int max, unsigned int *n, unsigned int lineno,
		    const char *fmt, ...) __attribute__((format(printf, 5, 6)));
static void problem(struct aworker_cfg_problem *out, unsigned int max, unsigned int *n, unsigned int lineno,
		    const char *fmt, ...)
{
	va_list ap;

	if (*n < max) {
		out[*n].lineno = lineno;
		va_start(ap, fmt);
		vsnprintf(out[*n].msg, sizeof(out[*n].msg), fmt, ap);
		va_end(ap);
	}
	(*n)++;
}

unsigned int aworker_cfg_model_check(const struct aworker_cfg_model *m, bool supervisor_mode,
				     struct aworker_cfg_problem *out, unsigned int max)
{
	unsigned int n = 0, i, j, nw = OSMO_MIN(m->num_workers, AWORKER_CFG_VIEW_MAX_WORKERS);
	int first_inline = -1, first_file = -1;

	/* One configuration form per worker; one form per pool (D-S1). */
	for (i = 0; i < nw; i++) {
		const struct aworker_cfg_decl *d = &m->decl[i];
		if (d->file && d->inl) {
			problem(out, max, &n, OSMO_MAX(d->file_lineno, d->inl_lineno),
				"worker %u is configured twice: 'worker %u config-file' (line %u) and 'worker %u"
				" cs7-instance' (line %u); a worker uses exactly one form", i, i, d->file_lineno, i,
				d->inl_lineno);
			continue;
		}
		if (d->inl && first_inline < 0)
			first_inline = i;
		if (d->file && first_file < 0)
			first_file = i;
	}
	if (supervisor_mode) {
		for (i = 0; i < nw; i++) {
			if (m->decl[i].file && !m->decl[i].inl)
				problem(out, max, &n, m->decl[i].file_lineno,
					"worker %u uses 'config-file', but the Supervisor accepts only the inline form"
					" 'worker %u cs7-instance <N>' with the cs7 instance in this file", i, i);
		}
	}
	if (first_inline < 0)
		return n;	/* no inline form: the file is parsed as a whole, as before */
	if (first_file >= 0)
		problem(out, max, &n, OSMO_MAX(m->decl[first_file].file_lineno, m->decl[first_inline].inl_lineno),
			"worker %d uses 'config-file' (line %u) and worker %d uses 'cs7-instance' (line %u); all"
			" workers of a pool use the same form", first_file, m->decl[first_file].file_lineno,
			first_inline, m->decl[first_inline].inl_lineno);

	/* Every worker-owned instance: exactly one section, with a transport of a single protocol. */
	for (i = 0; i < nw; i++) {
		const struct aworker_cfg_decl *d = &m->decl[i];
		const struct aworker_cfg_cs7 *c;

		if (!d->inl || (d->file && d->inl))
			continue;
		for (j = 0; j < i; j++) {
			if (m->decl[j].inl && !m->decl[j].file && m->decl[j].cs7_id == d->cs7_id)
				break;
		}
		if (j < i) {
			problem(out, max, &n, d->inl_lineno,
				"cs7 instance %u is assigned to worker %u (line %u) and to worker %u; each cs7 instance"
				" belongs to exactly one worker", d->cs7_id, j, m->decl[j].inl_lineno, i);
			continue;
		}
		c = &m->cs7[d->cs7_id];
		if (!c->num_sections) {
			problem(out, max, &n, d->inl_lineno,
				"worker %u owns cs7 instance %u, but this file has no 'cs7 instance %u' section", i,
				d->cs7_id, d->cs7_id);
			continue;
		}
		if (c->num_sections > 1)
			problem(out, max, &n, c->dup_lineno,
				"'cs7 instance %u' owned by worker %u appears more than once (first at line %u); a"
				" worker-owned instance is configured in one section", d->cs7_id, i, c->lineno);
		if (!c->num_transport)
			problem(out, max, &n, c->lineno,
				"'cs7 instance %u' is owned by worker %u but has no asp, as or listen node: the worker"
				" would own nothing", d->cs7_id, i);
		else if (c->proto_conflict)
			problem(out, max, &n, c->transport_lineno,
				"'cs7 instance %u' (owned by worker %u) names more than one ASP protocol", d->cs7_id, i);
	}

	for (i = 0; i < m->num_lines; i++) {
		const struct aworker_cfg_line *l = &m->line[i];
		bool owned = l->sec == AWORKER_CFG_SEC_CS7 && l->cs7_id >= 0
			     && aworker_cfg_model_owner(m, l->cs7_id) >= 0;
		bool relevant = owned || l->sec == AWORKER_CFG_SEC_POOL;

		if (l->bad_indent) {
			problem(out, max, &n, i + 1, "inconsistent indentation: leading whitespace must match the"
				" lines around it and reflect the node levels (the VTY parser refuses this line)");
			continue;
		}
		if (relevant && l->overlong)
			problem(out, max, &n, i + 1, "line longer than %u characters inside %s", VTY_BUFSIZ - 2,
				owned ? "a worker-owned cs7 instance" : "'a-worker-pool'");
		if (relevant && l->command && l->depth > 0 && l->exit_end)
			problem(out, max, &n, i + 1, "'exit' / 'end' inside %s: the node structure of this section"
				" must follow from the indentation alone", owned ? "a worker-owned cs7 instance"
										 : "'a-worker-pool'");
		if (l->transport_head && l->sec == AWORKER_CFG_SEC_CS7 && !owned) {
			if (l->cs7_id >= 0)
				problem(out, max, &n, i + 1,
					"asp, as and listen are only allowed in a cs7 instance owned by a worker, and"
					" no worker owns cs7 instance %d ('worker <N> cs7-instance %d'); with the inline"
					" form the BSC never runs a SIGTRAN transport itself", l->cs7_id, l->cs7_id);
			else
				problem(out, max, &n, i + 1,
					"asp, as and listen are only allowed in a cs7 instance owned by a worker, and"
					" this cs7 instance is not a valid instance number; with the inline form the BSC"
					" never runs a SIGTRAN transport itself");
		}
	}
	return n;
}

static bool keep_line(const struct aworker_cfg_model *m, const struct aworker_cfg_line *l,
		      enum aworker_cfg_view_role role, int own_cs7)
{
	switch (role) {
	case AWORKER_CFG_VIEW_BSC:
		/* Everything but the transport subtrees of the worker-owned instances. */
		return !(l->transport && l->sec == AWORKER_CFG_SEC_CS7 && l->cs7_id >= 0
			 && aworker_cfg_model_owner(m, l->cs7_id) >= 0);
	case AWORKER_CFG_VIEW_SUPERVISOR:
		return l->sec == AWORKER_CFG_SEC_LOG || l->sec == AWORKER_CFG_SEC_POOL;
	case AWORKER_CFG_VIEW_WORKER:
		/* D-2: never a file target (see aworker_cfg_view.h). */
		if (l->log_file)
			return false;
		return l->sec == AWORKER_CFG_SEC_LOG || l->no_log || l->sec == AWORKER_CFG_SEC_POOL
		       || (l->sec == AWORKER_CFG_SEC_CS7 && l->cs7_id >= 0 && l->cs7_id == own_cs7);
	}
	return false;
}

char *aworker_cfg_model_view(void *ctx, const struct aworker_cfg_model *m, enum aworker_cfg_view_role role,
			     unsigned int worker_idx, size_t *len)
{
	int own_cs7 = -1;
	char *s;
	unsigned int i;

	if (role == AWORKER_CFG_VIEW_WORKER) {
		if (worker_idx >= AWORKER_CFG_VIEW_MAX_WORKERS || !m->decl[worker_idx].inl)
			return NULL;
		own_cs7 = (int)m->decl[worker_idx].cs7_id;
	}
	s = talloc_strdup(ctx, "");
	OSMO_ASSERT(s);
	for (i = 0; i < m->num_lines; i++) {
		const struct aworker_cfg_line *l = &m->line[i];
		/* A removed line becomes an empty line, so that the view keeps the file's line numbers. */
		s = talloc_strdup_append_buffer(s, keep_line(m, l, role, own_cs7) ? l->text : "\n");
		OSMO_ASSERT(s);
	}
	/* D-2, defence in depth: whatever the model says, a worker view with a 'log file' command is not handed out. */
	if (role == AWORKER_CFG_VIEW_WORKER && aworker_cfg_view_log_file_line(s, strlen(s))) {
		talloc_free(s);
		return NULL;
	}
	if (len)
		*len = strlen(s);
	return s;
}

/* The tokens of one line, split as the parser splits them; NULL for a blank or comment line (or one it refuses). */
static vector line_tokens(const char *text)
{
	char *indent = NULL;
	vector v = NULL;

	if (cmd_make_strvec2(text, &indent, &v) != CMD_SUCCESS)
		v = NULL;
	talloc_free(indent);
	return v;
}

unsigned int aworker_cfg_check_worker_view(const char *text, size_t len, unsigned int worker_idx,
					   struct aworker_cfg_problem *out, unsigned int max)
{
	struct aworker_cfg_model *m;
	const struct aworker_cfg_decl *d = NULL;
	unsigned int n = 0, i, lf;
	int own = -1;

	/* Size (D-5). A view of the wrong size is not looked at any further. */
	if (!len) {
		problem(out, max, &n, 0, "the worker view is empty");
		return n;
	}
	if (len > AWORKER_CFG_SNAPSHOT_MAX) {
		problem(out, max, &n, 0, "the worker view has %zu octets, more than the %u a worker accepts", len,
			AWORKER_CFG_SNAPSHOT_MAX);
		return n;
	}
	/* A model of its own, read with the parser's rules: nothing the view builder decided is taken over. */
	if (aworker_cfg_model_build_buf(NULL, "worker view", text, len, &m) < 0) {
		problem(out, max, &n, 0, "cannot read the worker view");
		return n;
	}

	/* This worker's declaration: present, inline, and only inline. */
	if (!m->pool_lineno) {
		problem(out, max, &n, 0, "the worker view has no 'a-worker-pool' section, so it does not declare"
			" worker %u", worker_idx);
	} else if (worker_idx >= AWORKER_CFG_VIEW_MAX_WORKERS || worker_idx >= m->num_workers) {
		problem(out, max, &n, m->pool_lineno, "the 'a-worker-pool' section does not declare worker %u"
			" ('workers %u')", worker_idx, m->num_workers);
	} else {
		d = &m->decl[worker_idx];
		if (!d->inl)
			problem(out, max, &n, d->file ? d->file_lineno : m->pool_lineno,
				"worker %u is not declared with the inline form 'worker %u cs7-instance <X>'%s", worker_idx,
				worker_idx, d->file ? "; it is declared with 'config-file'" : "");
		else if (d->file)
			problem(out, max, &n, OSMO_MAX(d->file_lineno, d->inl_lineno),
				"worker %u is declared twice: 'worker %u config-file' (line %u) and 'worker %u"
				" cs7-instance' (line %u)", worker_idx, worker_idx, d->file_lineno, worker_idx,
				d->inl_lineno);
		else
			own = (int)d->cs7_id;
	}

	/* Its cs7 instance: exactly one section, with a transport. */
	if (own >= 0) {
		const struct aworker_cfg_cs7 *c = &m->cs7[own];
		if (!c->num_sections)
			problem(out, max, &n, d->inl_lineno, "worker %u owns cs7 instance %d, but the worker view has no"
				" 'cs7 instance %d' section", worker_idx, own, own);
		else if (c->num_sections > 1)
			problem(out, max, &n, c->dup_lineno, "'cs7 instance %d' appears more than once in the worker"
				" view (first at line %u)", own, c->lineno);
		if (c->num_sections && !c->num_transport)
			problem(out, max, &n, c->lineno, "'cs7 instance %d' has no asp, as or listen node: worker %u"
				" would own nothing", own, worker_idx);
	}

	/* Line by line. */
	for (i = 0; i < m->num_lines; i++) {
		const struct aworker_cfg_line *l = &m->line[i];
		vector v;

		if (l->bad_indent) {
			problem(out, max, &n, i + 1, "inconsistent indentation: leading whitespace must match the lines"
				" around it and reflect the node levels (the VTY parser refuses this line)");
			continue;
		}
		if (l->overlong)
			problem(out, max, &n, i + 1, "line longer than %u characters", VTY_BUFSIZ - 2);
		if (!l->command)
			continue;
		if (l->exit_end) {
			problem(out, max, &n, i + 1, "'exit' / 'end' in a worker view: its node structure must follow"
				" from the indentation alone");
			continue;
		}
		v = line_tokens(l->text);
		if (l->depth == 0) {
			if (l->sec == AWORKER_CFG_SEC_CS7 && (l->cs7_id < 0 || l->cs7_id != own)) {
				char owns[48];
				if (own >= 0)
					snprintf(owns, sizeof(owns), "it owns only cs7 instance %d", own);
				else
					snprintf(owns, sizeof(owns), "it owns none here");
				if (l->cs7_id < 0)
					problem(out, max, &n, i + 1, "a cs7 instance that is not a valid instance number"
						" (worker %u: %s)", worker_idx, owns);
				else
					problem(out, max, &n, i + 1, "cs7 instance %d is not worker %u's (%s)", l->cs7_id,
						worker_idx, owns);
			} else if (l->sec != AWORKER_CFG_SEC_CS7 && l->sec != AWORKER_CFG_SEC_LOG
				   && l->sec != AWORKER_CFG_SEC_POOL && !l->no_log) {
				problem(out, max, &n, i + 1, "'%s' is not part of a worker view (only 'log' targets,"
					" 'no log', 'a-worker-pool' and the worker's own cs7 instance are)",
					tok(v, 0) ? tok(v, 0) : "");
			}
		} else if (tok_is(v, 0, "log") || (tok_is(v, 0, "no") && tok_is(v, 1, "log"))) {
			problem(out, max, &n, i + 1, "'log' / 'no log' below another command: log targets are"
				" configured at the top level only");
		} else if ((tok_is(v, 0, "logging") || (tok_is(v, 0, "no") && tok_is(v, 1, "logging")))
			   && l->sec != AWORKER_CFG_SEC_LOG) {
			problem(out, max, &n, i + 1, "'logging ...' outside a 'log <target>' section");
		}
		if (v)
			cmd_free_strvec(v);
	}

	/* D-2: never a file target, at any depth. */
	lf = aworker_cfg_view_log_file_line(text, len);
	if (lf)
		problem(out, max, &n, lf, "'log file' / 'no log file' in a worker view: a worker never writes the BSC's"
			" log files (D-2)");

	talloc_free(m);
	return n;
}

unsigned int aworker_cfg_view_log_file_line(const char *text, size_t len)
{
	char buf[VTY_BUFSIZ];
	unsigned int lineno = 0, found = 0;
	FILE *f;

	if (!len)
		return 0;
	f = fmemopen((void *)text, len, "r");
	OSMO_ASSERT(f);
	/* Line by line as config_from_file() reads them; every command, whatever its depth. */
	while (!found && fgets(buf, sizeof(buf), f)) {
		char *indent = NULL;
		vector v = NULL;

		lineno++;
		if (cmd_make_strvec2(buf, &indent, &v) == CMD_SUCCESS && v
		    && ((tok_is(v, 0, "log") && tok_is(v, 1, "file"))
			|| (tok_is(v, 0, "no") && tok_is(v, 1, "log") && tok_is(v, 2, "file"))))
			found = lineno;
		if (v)
			cmd_free_strvec(v);
		talloc_free(indent);
	}
	fclose(f);
	return found;
}

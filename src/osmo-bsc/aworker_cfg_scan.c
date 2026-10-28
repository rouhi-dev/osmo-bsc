/* A-worker pool: config file scanner (see aworker_cfg_scan.h) */
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>

#include <osmocom/bsc/aworker_cfg_scan.h>

#define MAX_TOKENS	8
#define MAX_TOKEN_LEN	64

/* One split line: up to MAX_TOKENS whitespace separated tokens, each truncated to MAX_TOKEN_LEN-1 octets
 * (the tokens this scanner compares are all short keywords, numbers or protocol names). */
struct line {
	unsigned int indent;
	unsigned int n;
	char tok[MAX_TOKENS][MAX_TOKEN_LEN];
};

/* false for a blank or comment line. */
static bool split_line(const char *s, struct line *l)
{
	const char *p = s;

	l->n = 0;
	while (*p == ' ' || *p == '\t')
		p++;
	l->indent = (unsigned int)(p - s);
	if (!*p || *p == '\r' || *p == '\n' || *p == '!' || *p == '#')
		return false;

	while (*p && l->n < MAX_TOKENS) {
		size_t len = 0;
		char *dst = l->tok[l->n];

		while (p[len] && !isspace((unsigned char)p[len]))
			len++;
		if (!len)
			break;
		osmo_strlcpy(dst, p, OSMO_MIN(MAX_TOKEN_LEN, len + 1));
		l->n++;
		p += len;
		while (*p == ' ' || *p == '\t')
			p++;
	}
	return l->n > 0;
}

static bool is(const struct line *l, unsigned int i, const char *want)
{
	return i < l->n && !strcmp(l->tok[i], want);
}

/* OSMO_SS7_ASP_PROT_NONE if the token is not an ASP protocol name. */
static uint8_t proto_of(const struct line *l, unsigned int i)
{
	if (i >= l->n)
		return OSMO_SS7_ASP_PROT_NONE;
	if (!strcmp(l->tok[i], "m3ua"))
		return OSMO_SS7_ASP_PROT_M3UA;
	if (!strcmp(l->tok[i], "sua"))
		return OSMO_SS7_ASP_PROT_SUA;
	if (!strcmp(l->tok[i], "ipa"))
		return OSMO_SS7_ASP_PROT_IPA;
	return OSMO_SS7_ASP_PROT_NONE;
}

static void note_sigtran_node(struct aworker_cfg_scan_cs7 *c, const char *node, unsigned int lineno,
			      uint8_t proto)
{
	if (!c->node[0]) {
		osmo_strlcpy(c->node, node, sizeof(c->node));
		c->node_lineno = lineno;
	}
	if (proto == OSMO_SS7_ASP_PROT_NONE)
		return;
	if (c->asp_proto == OSMO_SS7_ASP_PROT_NONE)
		c->asp_proto = proto;
	else if (c->asp_proto != proto)
		c->proto_conflict = true;
}

int aworker_cfg_scan(const char *config_file, struct aworker_cfg_scan *out)
{
	struct aworker_cfg_scan_cs7 *cur = NULL;
	unsigned int cur_indent = 0, pool_indent = 0;
	bool in_pool = false;
	unsigned int lineno = 0;
	char buf[1024];
	struct line l;
	FILE *f;

	memset(out, 0, sizeof(*out));

	f = fopen(config_file, "r");
	if (!f)
		return -errno;

	while (fgets(buf, sizeof(buf), f)) {
		lineno++;
		if (!split_line(buf, &l))
			continue;

		/* A line at or left of a section's own indentation ends that section. */
		if (cur && l.indent <= cur_indent)
			cur = NULL;
		if (in_pool && l.indent <= pool_indent)
			in_pool = false;

		if (!cur && !in_pool) {
			if (is(&l, 0, "cs7") && is(&l, 1, "instance") && l.n >= 3) {
				if (out->num_cs7 >= AWORKER_CFG_SCAN_MAX_CS7) {
					out->cs7_overflow = true;
					continue;
				}
				cur = &out->cs7[out->num_cs7++];
				cur->id = (uint32_t)strtoul(l.tok[2], NULL, 10);
				cur->lineno = lineno;
				cur_indent = l.indent;
				continue;
			}
			if (is(&l, 0, "a-worker-pool")) {
				in_pool = true;
				out->have_pool_section = true;
				pool_indent = l.indent;
				continue;
			}
			continue;
		}

		if (in_pool) {
			if (is(&l, 0, "workers") && l.n >= 2)
				out->num_workers = (unsigned int)strtoul(l.tok[1], NULL, 10);
			continue;
		}

		/* Inside a cs7 instance. 'asp NAME <rport> <lport> PROTO', 'as NAME PROTO' and
		 * 'listen PROTO <port>' are the three nodes that make this process own a SIGTRAN stack. */
		if (is(&l, 0, "asp")) {
			cur->num_asp++;
			note_sigtran_node(cur, "asp", lineno, proto_of(&l, 4));
		} else if (is(&l, 0, "as")) {
			cur->num_as++;
			note_sigtran_node(cur, "as", lineno, proto_of(&l, 2));
		} else if (is(&l, 0, "listen")) {
			cur->num_listen++;
			note_sigtran_node(cur, "listen", lineno, proto_of(&l, 1));
		}
	}
	fclose(f);
	return 0;
}

const struct aworker_cfg_scan_cs7 *aworker_cfg_scan_find(const struct aworker_cfg_scan *scan, uint32_t cs7_id)
{
	unsigned int i;

	for (i = 0; i < scan->num_cs7; i++) {
		if (scan->cs7[i].id == cs7_id)
			return &scan->cs7[i];
	}
	return NULL;
}

const struct aworker_cfg_scan_cs7 *aworker_cfg_scan_first_sigtran(const struct aworker_cfg_scan *scan)
{
	unsigned int i;

	for (i = 0; i < scan->num_cs7; i++) {
		const struct aworker_cfg_scan_cs7 *c = &scan->cs7[i];
		if (c->num_asp || c->num_as || c->num_listen)
			return c;
	}
	return NULL;
}

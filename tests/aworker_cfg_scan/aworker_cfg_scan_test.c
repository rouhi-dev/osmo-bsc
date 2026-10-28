/* Tests for the A-worker config file scanner (aworker_cfg_scan.c) */
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

/* The scanner decides two things that must be right before any configuration file is parsed: whether worker mode
 * is configured, and whether the file would make the reading process own a SIGTRAN stack. Each case below is
 * written to a temporary file and scanned exactly as osmo-bsc does it. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>

#include <osmocom/bsc/aworker_cfg_scan.h>

static void *ctx;
static char tmpl[] = "/tmp/aworker_cfg_scan_test.XXXXXX";
static char *path;

static void write_cfg(const char *content)
{
	FILE *f = fopen(path, "w");
	OSMO_ASSERT(f);
	OSMO_ASSERT(fputs(content, f) >= 0);
	fclose(f);
}

static const char *proto_name(uint8_t p)
{
	return p == OSMO_SS7_ASP_PROT_NONE ? "none" : osmo_ss7_asp_protocol_name(p);
}

static void dump(const char *title, const char *content)
{
	struct aworker_cfg_scan scan;
	const struct aworker_cfg_scan_cs7 *first;
	unsigned int i;
	int rc;

	printf("\n=== %s ===\n", title);
	write_cfg(content);
	rc = aworker_cfg_scan(path, &scan);
	printf("rc=%d\n", rc);
	if (rc < 0)
		return;
	printf("pool section=%d workers=%u cs7 sections=%u overflow=%d\n", scan.have_pool_section,
	       scan.num_workers, scan.num_cs7, scan.cs7_overflow);
	for (i = 0; i < scan.num_cs7; i++) {
		const struct aworker_cfg_scan_cs7 *c = &scan.cs7[i];
		printf("  cs7 instance %u (line %u): asp=%u as=%u listen=%u proto=%s conflict=%d"
		       " first-node='%s' (line %u)\n", c->id, c->lineno, c->num_asp, c->num_as, c->num_listen,
		       proto_name(c->asp_proto), c->proto_conflict, c->node, c->node_lineno);
	}
	first = aworker_cfg_scan_first_sigtran(&scan);
	printf("first sigtran section: %s\n", first ? "yes" : "none");
	if (first)
		printf("  -> cs7 instance %u, node '%s' on line %u\n", first->id, first->node,
		       first->node_lineno);
	printf("find(0)=%s find(3)=%s find(9)=%s\n",
	       aworker_cfg_scan_find(&scan, 0) ? "yes" : "no",
	       aworker_cfg_scan_find(&scan, 3) ? "yes" : "no",
	       aworker_cfg_scan_find(&scan, 9) ? "yes" : "no");
}

int main(void)
{
	int fd;

	ctx = talloc_named_const(NULL, 0, "aworker_cfg_scan_test");
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, NULL);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	log_set_print_category(osmo_stderr_target, 0);

	path = tmpl;
	fd = mkstemp(path);
	OSMO_ASSERT(fd >= 0);
	close(fd);

	/* The in-process configuration osmo-bsc has always used: one cs7 instance with an asp, no pool. The
	 * scanner reports the asp, but with no 'a-worker-pool' section nothing is restricted. */
	dump("in-process config (asp, no pool)",
	     "line vty\n"
	     " no login\n"
	     "cs7 instance 0\n"
	     " point-code 0.23.3\n"
	     " asp asp-bsc-to-stp 2905 0 m3ua tcp\n"
	     "  remote-ip 127.0.0.1\n"
	     "  role asp\n"
	     "  transport-role client\n"
	     " sccp-address msc\n"
	     "  routing-indicator PC\n"
	     "  point-code 0.23.1\n"
	     "msc 0\n"
	     " msc-addr msc\n");

	/* A main configuration for worker mode: cs7 keeps only the inert parts. */
	dump("worker-mode main config (no asp)",
	     "cs7 instance 0\n"
	     " point-code 0.23.3\n"
	     " sccp-address msc\n"
	     "  routing-indicator PC\n"
	     "  point-code 0.23.1\n"
	     "a-worker-pool\n"
	     " workers 2\n"
	     " worker 0 config-file /etc/osmocom/aw0.cfg\n"
	     " worker 1 config-file /etc/osmocom/aw1.cfg\n"
	     "msc 0\n"
	     " msc-addr msc\n");

	/* The case that must be refused: worker mode and an asp in the main file. */
	dump("worker-mode main config with an asp (must be refused)",
	     "a-worker-pool\n"
	     " workers 1\n"
	     "cs7 instance 0\n"
	     " point-code 0.23.3\n"
	     " asp my-asp 2905 0 m3ua\n"
	     "  role asp\n"
	     "msc 0\n");

	/* 'as' and 'listen' are equally disqualifying, and both carry the protocol. */
	dump("as node", "a-worker-pool\n workers 1\ncs7 instance 1\n as my-as m3ua\n  asp my-asp\n");
	dump("listen node", "a-worker-pool\n workers 1\ncs7 instance 2\n listen m3ua 2905\n  accept-asp-connections dynamic-permitted\n");

	/* A worker configuration: several cs7 instances, counted and typed. */
	dump("worker config with two cs7 instances",
	     "cs7 instance 0\n"
	     " point-code 0.23.3\n"
	     " asp a0 2905 0 m3ua tcp\n"
	     "  role asp\n"
	     "  transport-role client\n"
	     "  no shutdown\n"
	     " as as0 m3ua\n"
	     "  asp a0\n"
	     "cs7 instance 3\n"
	     " point-code 0.23.4\n"
	     " asp a1 5000 0 ipa\n"
	     "  role asp\n");

	/* Two protocols in one cs7 instance: the A interface of one instance speaks one protocol. */
	dump("protocol conflict",
	     "cs7 instance 0\n"
	     " asp a0 2905 0 m3ua\n"
	     " asp a1 14001 0 sua\n");

	/* Comments, blank lines, tabs and trailing whitespace must not confuse the scanner, and a node outside a
	 * cs7 instance (here: 'as' at top level, which is not a command osmo-bsc has) must not be attributed to
	 * one. */
	dump("comments, blank lines and indentation",
	     "! a comment\n"
	     "\n"
	     "# another comment\n"
	     "cs7 instance 0   \n"
	     "\t point-code 0.23.3\n"
	     "\t asp a0 2905 0 m3ua tcp   \n"
	     "\t  role asp\n"
	     "!  asp commented-out 1 2 m3ua\n"
	     "network\n"
	     " bts 0\n"
	     "  type osmo-bts\n"
	     "a-worker-pool\n"
	     " workers 1\n");

	/* 'workers' outside the pool section, and a cs7 section that ends before it. */
	dump("workers outside the a-worker-pool section",
	     "cs7 instance 0\n"
	     " point-code 0.23.3\n"
	     "network\n"
	     " workers 7\n");

	/* An unreadable file is reported, not guessed at. */
	printf("\n=== missing file ===\n");
	{
		struct aworker_cfg_scan scan;
		int rc = aworker_cfg_scan("/nonexistent/osmo-bsc.cfg", &scan);
		printf("rc=%d (%s)\n", rc, rc == -ENOENT ? "ENOENT" : "unexpected");
	}

	/* An empty file: no pool, no cs7, no restriction. */
	dump("empty file", "");

	unlink(path);
	printf("\nDone\n");
	talloc_free(ctx);
	return 0;
}

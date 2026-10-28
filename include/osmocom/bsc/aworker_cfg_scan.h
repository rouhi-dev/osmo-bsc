/* A-worker pool: a read-only scanner for the 'cs7' and 'a-worker-pool' sections of a config file */
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
#pragma once

/* WHY A SCANNER
 * =============
 *
 * The A-worker pool needs two answers before a configuration file is handed to the VTY parser:
 *
 * - In the main process: does a 'cs7 instance' contain an 'asp', 'as' or 'listen' node? Parsing one of those
 *   creates the ASP and opens its SCTP association right away (ss7_vty_node_asp_go_parent() and the 'asp' node's
 *   '[no] shutdown' commands in libosmo-sigtran call osmo_ss7_asp_restart()), and the pinned libosmo-sigtran
 *   offers no way to suppress that. The only way to keep the SIGTRAN stack out of the main process is therefore
 *   to refuse such a file before reading it.
 *
 * - In a worker: which cs7 instances does this worker own, and with which ASP protocol? The public
 *   libosmo-sigtran API can enumerate Application Servers (osmo_ss7_as_find_by_proto()) but not ASPs, and the
 *   usual osmo-bsc configuration defines only an 'asp' and lets osmo_sccp_simple_client_on_ss7_id() create the
 *   AS. Reading it from the file covers both spellings.
 *
 * The scanner is read-only: it never configures anything, and the VTY parser remains the single source of the
 * actual configuration. It understands the osmo-bsc configuration file layout -- one command per line, child
 * nodes indented, '!' and '#' comment lines -- and the four command syntaxes it looks for:
 *
 *   cs7 instance <0-15>
 *    asp NAME <0-65535> <0-65535> (sua|m3ua|ipa) [(sctp|tcp)]
 *    as NAME (sua|m3ua|ipa)
 *    listen (sua|m3ua|ipa) <0-65534> [(sctp|tcp)]
 *   a-worker-pool
 *    workers <0-16>
 */

#include <stdbool.h>
#include <stdint.h>

#define AWORKER_CFG_SCAN_MAX_CS7	16

/* One 'cs7 instance' section found in the file. */
struct aworker_cfg_scan_cs7 {
	uint32_t id;
	unsigned int lineno;		/*!< line of the 'cs7 instance' command */
	unsigned int num_asp;
	unsigned int num_as;
	unsigned int num_listen;
	/*! enum osmo_ss7_asp_protocol of the asp / as / listen nodes; OSMO_SS7_ASP_PROT_NONE (0) if the section
	 * has none, and the protocol of the first one if they disagree (see proto_conflict). */
	uint8_t asp_proto;
	bool proto_conflict;		/*!< the section names more than one ASP protocol */
	/*! Line and name of the first asp / as / listen node, for error messages. */
	unsigned int node_lineno;
	char node[16];
};

struct aworker_cfg_scan {
	unsigned int num_workers;	/*!< 'workers' in the 'a-worker-pool' section; 0 if absent */
	bool have_pool_section;
	unsigned int num_cs7;		/*!< entries in cs7[] */
	bool cs7_overflow;		/*!< more than AWORKER_CFG_SCAN_MAX_CS7 sections (the rest is ignored) */
	struct aworker_cfg_scan_cs7 cs7[AWORKER_CFG_SCAN_MAX_CS7];
};

/*! Scan a configuration file.
 * \param[in] config_file  path of the file.
 * \param[out] out  the result; fully overwritten.
 * \returns 0, or -errno if the file cannot be read. */
int aworker_cfg_scan(const char *config_file, struct aworker_cfg_scan *out);

/*! The scanned section with this cs7 instance id, or NULL. */
const struct aworker_cfg_scan_cs7 *aworker_cfg_scan_find(const struct aworker_cfg_scan *scan, uint32_t cs7_id);

/*! The first cs7 section that has an asp, as or listen node, or NULL if none has. */
const struct aworker_cfg_scan_cs7 *aworker_cfg_scan_first_sigtran(const struct aworker_cfg_scan *scan);

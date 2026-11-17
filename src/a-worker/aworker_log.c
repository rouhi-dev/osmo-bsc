/* osmo-bsc log categories, shared by every program built from this tree, and the A-worker log category */
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

#include <osmocom/core/logging.h>
#include <osmocom/core/utils.h>

#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/debug.h>

int aworker_log_cat = DAWORKER;

const struct log_info_cat osmo_bsc_categories[Debug_LastEntry] = {
	[DRLL] = {
		.name = "DRLL",
		.description = "A-bis Radio Link Layer (RLL)",
		.color = "\033[1;31m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DMM] = {
		.name = "DMM",
		.description = "Layer3 Mobility Management (MM)",
		.color = "\033[1;33m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DRR] = {
		.name = "DRR",
		.description = "Layer3 Radio Resource (RR)",
		.color = "\033[1;34m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DRSL] = {
		.name = "DRSL",
		.description = "A-bis Radio Signalling Link (RSL)",
		.color = "\033[1;35m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DCHAN] = {
		.name = "DCHAN",
		.description = "lchan FSM",
		.color = "\033[1;32m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DTS] = {
		.name = "DTS",
		.description = "timeslot FSM",
		.color = "\033[1;31m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DAS] = {
		.name = "DAS",
		.description = "assignment FSM",
		.color = "\033[1;33m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DNM] =	{
		.name = "DNM",
		.description = "A-bis Network Management / O&M (NM/OML)",
		.color = "\033[1;36m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DPAG]	= {
		.name = "DPAG",
		.description = "Paging Subsystem",
		.color = "\033[1;38m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DMEAS] = {
		.name = "DMEAS",
		.description = "Radio Measurement Processing",
		.enabled = 0, .loglevel = LOGL_NOTICE,
	},
	[DMSC] = {
		.name = "DMSC",
		.description = "Mobile Switching Center",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DHO] = {
		.name = "DHO",
		.description = "Hand-Over Process",
		.color = "\033[1;38m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DHODEC] = {
		.name = "DHODEC",
		.description = "Hand-Over Decision",
		.color = "\033[1;38m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DREF] = {
		.name = "DREF",
		.description = "Reference Counting",
		.enabled = 0, .loglevel = LOGL_NOTICE,
	},
	[DCTRL] = {
		.name = "DCTRL",
		.description = "Control interface",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DFILTER] = {
		.name = "DFILTER",
		.description = "BSC/NAT IMSI based filtering",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DPCU] = {
		.name = "DPCU",
		.description = "PCU Interface",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DLCLS] = {
		.name = "DLCLS",
		.description = "Local Call, Local Switch",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DCBS] = {
		.name = "DCBS",
		.description = "Cell Broadcast System",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DLCS] = {
		.name = "DLCS",
		.description = "Location Services",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DASCI] = {
		.name = "DASCI",
		.description = "Advanced Speech Call Items (VGCS/VBS)",
		.color = "\033[1;38m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DRESET] = {
		.name = "DRESET",
		.description = "RESET/ACK on A and Lb interfaces",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DLOOP] = {
		.name = "DLOOP",
		.description = "Control loops",
		.color = "\033[0;34m",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
	[DAWORKER] = {
		.name = "DAWORKER",
		.description = "A-interface worker pool (SIGTRAN worker processes and their IPC)",
		.enabled = 1, .loglevel = LOGL_NOTICE,
	},
};

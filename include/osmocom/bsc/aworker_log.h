/* osmo-bsc log categories and the A-worker log category */
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

#include <osmocom/core/logging.h>

#include <osmocom/bsc/debug.h>

/*! The osmo-bsc log category table (indexed by the enum in debug.h), shared by every program built from this
 * tree, so that the same 'logging level' lines are understood everywhere. */
extern const struct log_info_cat osmo_bsc_categories[Debug_LastEntry];

/*! Log category of the A-worker code (life cycle, worker role, IPC). DAWORKER unless the program sets another. */
extern int aworker_log_cat;

/* IDENTITY (decision B5): the program that runs the A-worker code names itself in its messages, and bsc-aworker marks
 * every line with its role. osmo-bsc sets nothing, so its output stays exactly what it was. */

/*! The program, as named in messages and in the IPC HELLO: "osmo-bsc" unless set (bsc-aworker: "bsc-aworker"). */
extern const char *aworker_program_name;
/*! Prefix of the pool's (main process / Supervisor) log lines: "" unless set (bsc-aworker: "aworker-supervisor: "). */
extern const char *aworker_log_prefix;
/*! The worker process' incarnation: its start number in the pool, 0 if unknown. bsc-aworker's Supervisor passes it to
 * each worker it starts (--aworker-incarnation), whose lines then begin "aworker <idx> inc <n>: " instead of
 * "aworker <idx>: ". */
extern unsigned int aworker_incarnation;

/*! LOGP() in the A-worker category with aworker_log_prefix in front. */
#define LOGPAW(lvl, fmt, args...) LOGP(aworker_log_cat, lvl, "%s" fmt, aworker_log_prefix, ## args)

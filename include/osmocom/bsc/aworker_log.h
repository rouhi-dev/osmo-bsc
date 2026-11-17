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

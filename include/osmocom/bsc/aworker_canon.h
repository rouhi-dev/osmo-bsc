/* A-worker: the canonical configuration document (decision CD-1, D-S5) */
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

/* WHAT IS COMPARED
 * ================
 *
 * The BSC and the Supervisor read the same osmo-bsc.cfg independently. Before a BSC session is admitted, the
 * Supervisor compares the BSC's canonical document with its own, byte for byte (D-S5: no hash). The document is built
 * by this code in both processes, from what each of them parsed; raw files are never compared. Text, one line per
 * item, each ending in '\n'; three parts (CD-1, as proposed in PHASE2_SUPERVISOR.md §4.6 and approved for S2-5):
 *
 *   H  "aworker-canonical-config 1"
 *      The representation version. ANY change to what this file writes -- a key added, an order or a format changed --
 *      changes it; a different H is a configuration mismatch of its own.
 *   P  "P <key> <value...>", the EFFECTIVE pool definition: every Supervisor- and worker-relevant 'a-worker-pool' value
 *      after parsing, defaults written out, in this fixed order:
 *        workers N
 *        worker I cs7-instance X            (one per worker, ascending I)
 *        supervisor-socket PATH | none
 *        startup-timeout, restart-delay, restart-delay-max, stable-time S
 *        max-restarts N window S
 *        on-worker-failure restart|shutdown
 *        session-echo-interval S
 *        session-echo-misses N
 *      Integers in decimal, enumerations by their VTY keyword. BSC-only keys (none exists yet; drain-timeout and the
 *      admission keys when they come) are excluded, so that BSC-only tuning never forces a pool restart.
 *   C  "C <worker> <depth> <token> <token>...", the worker-owned cs7 subtrees: for each worker in ascending index,
 *      every command line of its 'cs7 instance X' section(s) of the file, the head included, in file order (VTY
 *      order is significant). depth is the node depth the parser derives from the indentation; the tokens are those
 *      of libosmovty's cmd_make_strvec2(), joined by one space. Comments, blank lines and the indentation characters
 *      are dropped.
 *
 * C is SYNTACTIC, not effective (CD-1 accepts this): defaults omitted in the file stay implicit, and two spellings of
 * the same value (a point code in another format) differ. Such a difference makes a FALSE MISMATCH -- a refusal,
 * which is safe -- never a false match. Effective cs7 facts are cross-checked at runtime (CS7_INFO).
 *
 * The file form ('worker N config-file FILE') has no canonical document: supervisor mode needs the inline form.
 */

#include <stddef.h>

struct aworker_pool_cfg;
struct aworker_cfg_model;

#define AWORKER_CANON_VERSION	1
#define AWORKER_CANON_HEADER	"aworker-canonical-config"
/*! Largest document either side builds or accepts (SESSION_OPEN passes it in a sealed memfd). */
#define AWORKER_CANON_MAX	(512 * 1024)

/*! Build the canonical document of a pool configuration (cfg, as parsed) and the main configuration file it came from
 * (m: the cs7 subtrees of the workers' instances). NUL-terminated; *len excludes the NUL.
 * \returns the document (talloc'ed under ctx), or NULL: a worker uses the file form, a line cannot be represented
 * (overlong, or a tokenizer error), the document would exceed AWORKER_CANON_MAX, or out of memory (why says which). */
char *aworker_canon_build(void *ctx, const struct aworker_pool_cfg *cfg, const struct aworker_cfg_model *m, size_t *len,
			  char *why, size_t why_len);

/*! Compare two documents exactly. \returns 0 if they are identical; otherwise the 1-based number of the first line
 * that differs (a line missing on one side counts), with a description of it in why: the part (H, P, C), for C the
 * worker, and both lines as they are, the first as "ours". */
unsigned int aworker_canon_compare(const char *ours, size_t ours_len, const char *theirs, size_t theirs_len,
				   char *why, size_t why_len);

/*! The canonical document of the configuration this process loaded (aworker_pool_supervisor_read_config(), or
 * osmo-bsc's aworker_pool_read_config() with the inline form). \returns NULL without one (as aworker_canon_build()). */
char *aworker_pool_canon_document(void *ctx, size_t *len, char *why, size_t why_len);

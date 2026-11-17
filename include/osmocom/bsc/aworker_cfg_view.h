/* A-worker pool: process-specific views of osmo-bsc.cfg (inline worker configuration) */
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

/* VIEWS
 * =====
 *
 * With the inline form ('worker <N> cs7-instance <X>' in 'a-worker-pool'), osmo-bsc.cfg holds the worker-owned cs7
 * instances complete with their asp / as / listen nodes. No process may parse all of it: parsing an asp, as or listen
 * node creates the ASP and opens its transport at once (see aworker_cfg_scan.h), so each process parses a view:
 *
 *  - BSC: the whole file, except the asp / as / listen subtrees (and their 'no' forms) of the worker-owned cs7
 *    instances;
 *  - Supervisor: the 'log' sections and the 'a-worker-pool' section;
 *  - worker N: the 'log' sections except 'log file' targets, the top-level 'no log <target>' commands except 'no log
 *    file', the 'a-worker-pool' section and the 'cs7 instance' section it owns.
 *
 * Worker logging (decision D-2): a worker never writes the BSC's log files. A 'log file FILENAME' command and every
 * line indented below it (the settings of that one target: 'logging level ...', filters, 'logging print ...',
 * 'logging timestamp ...', ...), and every 'no log file FILENAME', are left out of a worker's view; the worker keeps
 * its stderr target and the worker logging options of its command line. All other targets are kept as they are:
 * stderr, syslog, systemd-journal, gsmtap, alarms and emscripten, and their 'no log ...' forms. This is exact, not a
 * guess at the parser: libosmovty runs a 'logging ...' line of a file only in the node of the 'log ...' command it is
 * indented below (CFG_LOG_NODE, whose target is that command's), and nowhere else; at the top level the line is
 * refused. 'no log file' is left out with its target, as it would fail without it ("No such log file").
 *
 * A view only selects whole lines; the standard VTY parser interprets every value (vty_read_config_filep()). Removed
 * lines are replaced by empty lines, which the parser skips, so a view has the line numbers of the file.
 *
 * The file is read with the parser's own rules: fgets() into VTY_BUFSIZ, cmd_make_strvec2() for the tokens and the
 * indentation, and indentation compared like libosmovty's indent_cmp() (the indent of a line is either equal to, a
 * proper prefix of, or an extension of another, or the file is invalid). Commands are matched by exact keywords, as
 * the parser does for files (cmd_execute_command_strict() with EXACT_MATCH). A line indented deeper than the line
 * before is taken as a child of that line: the parser accepts deeper indentation only after a command that entered a
 * node, so in every file the parser accepts, this is exactly the node structure.
 *
 * The rules for the inline form (aworker_cfg_model_check()) are those of docs/aworker/PHASE2_SUPERVISOR.md §4.2 and
 * §4.5 (decision D-S1). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AWORKER_CFG_VIEW_MAX_WORKERS	16
#define AWORKER_CFG_VIEW_MAX_CS7	16

enum aworker_cfg_view_role {
	AWORKER_CFG_VIEW_BSC,
	AWORKER_CFG_VIEW_SUPERVISOR,
	AWORKER_CFG_VIEW_WORKER,
};

/*! Top-level section a line belongs to. */
enum aworker_cfg_section {
	AWORKER_CFG_SEC_NONE,		/*!< before the first command */
	AWORKER_CFG_SEC_OTHER,		/*!< any other top-level node (network, msc, line vty, ...) */
	AWORKER_CFG_SEC_LOG,		/*!< 'log ...' */
	AWORKER_CFG_SEC_POOL,		/*!< 'a-worker-pool' */
	AWORKER_CFG_SEC_CS7,		/*!< 'cs7 instance <N>' */
};

/*! One line of the file, as the parser reads it (one fgets() call). */
struct aworker_cfg_line {
	char *text;			/*!< exactly as read, including the newline if there was one */
	bool overlong;			/*!< the line did not fit into VTY_BUFSIZ; the parser reads the rest as another line */
	bool command;			/*!< false for a blank or comment line */
	bool bad_indent;		/*!< the parser would refuse this line's indentation */
	unsigned int depth;		/*!< node depth of a command line (0: top level) */
	enum aworker_cfg_section sec;	/*!< for blank and comment lines: that of the command before */
	int cs7_id;			/*!< AWORKER_CFG_SEC_CS7: the instance, -1 if not a number below MAX_CS7 */
	bool transport;			/*!< inside an asp / as / listen subtree directly below a cs7 instance */
	bool transport_head;		/*!< the asp / as / listen (or 'no ...') command itself */
	bool exit_end;			/*!< the command is 'exit' or 'end' */
	bool log_file;			/*!< in a top-level 'log file' command's section (the command and everything
					 *   below it), or a top-level 'no log file' command (and anything below it).
					 *   The one place that decides "file target" for both forms (D-2). */
	bool no_log;			/*!< a top-level 'no log <target>' command, any target (and anything below
					 *   it). A 'no log file' line has both flags; a worker view checks log_file
					 *   first, so it is left out. */
};

/*! A worker's declaration in 'a-worker-pool'. */
struct aworker_cfg_decl {
	bool file;			/*!< 'worker N config-file FILE' */
	unsigned int file_lineno;
	bool inl;			/*!< 'worker N cs7-instance X' */
	unsigned int inl_lineno;
	uint32_t cs7_id;
};

/*! A 'cs7 instance <N>' section of the file. */
struct aworker_cfg_cs7 {
	unsigned int num_sections;	/*!< how often 'cs7 instance N' appears */
	unsigned int lineno;		/*!< the first one */
	unsigned int dup_lineno;	/*!< the second one, if any */
	unsigned int num_transport;	/*!< asp / as / listen commands directly below it */
	unsigned int transport_lineno;	/*!< the first of them */
	char transport[16];		/*!< its keyword */
	uint8_t proto;			/*!< enum osmo_ss7_asp_protocol of the first one naming one */
	bool proto_conflict;		/*!< more than one protocol named */
};

/*! The file, split into lines and sections. */
struct aworker_cfg_model {
	const char *file;
	unsigned int num_lines;
	struct aworker_cfg_line *line;	/*!< line[0] is line 1 of the file */
	unsigned int num_workers;	/*!< 'workers N' of the 'a-worker-pool' section (the last one), 0 if none */
	unsigned int pool_lineno;	/*!< line of the 'a-worker-pool' command, 0 if none */
	struct aworker_cfg_decl decl[AWORKER_CFG_VIEW_MAX_WORKERS];
	struct aworker_cfg_cs7 cs7[AWORKER_CFG_VIEW_MAX_CS7];
};

/*! One problem found by aworker_cfg_model_check(), naming the line it is about. */
struct aworker_cfg_problem {
	unsigned int lineno;		/*!< 0: not tied to one line */
	char msg[320];
};

/*! Read \a file into a model (talloc'ed under \a ctx). \returns 0, or -errno if the file cannot be read. */
int aworker_cfg_model_build(void *ctx, const char *file, struct aworker_cfg_model **out);
/*! The model of a configuration held in memory, read with exactly the same rules; \a name is used in diagnostics.
 * \returns 0, or -errno. */
int aworker_cfg_model_build_buf(void *ctx, const char *name, const char *buf, size_t len,
				struct aworker_cfg_model **out);

/* READING THE SOURCE CONFIGURATION ONCE
 * ====================================
 *
 * With an A-worker pool, osmo-bsc.cfg is opened and read exactly once. The model, the BSC's parse and (S2-2) every
 * worker snapshot are made from that one buffer, so they cannot disagree because the file changed between two reads.
 *
 * A change WHILE the file is read is detected on a best-effort basis, not prevented: no user-space method gives an
 * atomic snapshot of a file that another process writes in place without cooperating (locking). The read is refused
 * if, between before and after, the open file's device, inode, size, modification time or change time (nanoseconds)
 * changed, if the path no longer names that inode, if the file ended early or went on past its size, or if a second
 * pass over the contents differs from the first. A write that changes none of these (for example within one timestamp
 * tick, restoring the same size, before the second pass reads that region) is not detected. */

/*! The first line of a configuration text that is a 'log file' or 'no log file' command, at any depth, read with the
 * parser's rules; 0 if there is none. A worker's view must have none (D-2): aworker_cfg_model_view() checks every
 * worker view it makes with this and returns NULL rather than such a view. */
unsigned int aworker_cfg_view_log_file_line(const char *text, size_t len);

/* WORKER-VIEW CHECK (D-13)
 * ========================
 *
 * A worker in snapshot mode does not take its view on trust: before anything of it is parsed (parsing an asp, as or
 * listen node starts that transport at once), the text is checked with aworker_cfg_check_worker_view(), from a model
 * of its own built with the parser's rules (aworker_cfg_model_build_buf()), independent of the view builder:
 *
 *  - size: not empty, at most AWORKER_CFG_SNAPSHOT_MAX octets (D-5);
 *  - the 'a-worker-pool' section declares this worker with the inline form 'worker <N> cs7-instance <X>', and only
 *    with it;
 *  - exactly one 'cs7 instance <X>' section, with at least one asp, as or listen node;
 *  - no other cs7 instance (another worker's, or an invalid number), and no other section than those a worker view
 *    is made of: 'log <target>' sections, top-level 'no log <target>' commands, 'a-worker-pool' and its own cs7
 *    instance (see VIEWS above);
 *  - no 'log file' / 'no log file' at any depth (D-2); 'log' / 'no log' only at the top level; 'logging ...' only
 *    inside a 'log <target>' section;
 *  - no line the parser would refuse for its indentation, no line longer than the parser reads at once, and no
 *    'exit' / 'end' (the node structure must follow from the indentation alone).
 *
 * Every problem is reported with its line; a view with any problem must not be parsed at all. */

/*! Largest worker view (snapshot) accepted (D-5): exactly this many octets are accepted, one more is refused. */
#define AWORKER_CFG_SNAPSHOT_MAX	(1024u * 1024)

/*! Check \a len octets of \a text as the view of worker \a worker_idx (D-13, see above), before it is parsed.
 * \returns the number of problems found (0: the view may be parsed); at most \a max of them are written to \a out. */
unsigned int aworker_cfg_check_worker_view(const char *text, size_t len, unsigned int worker_idx,
					   struct aworker_cfg_problem *out, unsigned int max);

/*! Largest source configuration accepted (U-2). Each worker snapshot has its own, smaller limit. */
#define AWORKER_CFG_SOURCE_MAX	(16u * 1024 * 1024)

/*! A configuration file read once (aworker_cfg_read_stable()). */
struct aworker_cfg_source {
	char *path;			/*!< the path it was read from */
	char *buf;			/*!< its contents, len octets (plus a terminating NUL that is not part of it) */
	size_t len;
};

/*! Open \a path once and read it completely into memory (talloc'ed under \a ctx), detecting changes made while it is
 * read (see above). \returns 0; -errno if it cannot be opened or read; -EINVAL if it is not a regular file; -EFBIG if
 * it is larger than \a max octets; -ESTALE if it changed while it was read. */
int aworker_cfg_read_stable(void *ctx, const char *path, size_t max, struct aworker_cfg_source **out);
/*! Tests only: where aworker_cfg_read_stable() calls the test hook. */
enum aworker_cfg_read_stable_stage {
	AWORKER_CFG_READ_STABLE_BEFORE_OPEN,	/*!< after the path was found to be a regular file (buf is NULL) */
	AWORKER_CFG_READ_STABLE_AFTER_READ,	/*!< after the read, before the checks (the hook may alter buf) */
};
/*! Tests only: install (or with NULL remove) the test hook. */
void aworker_cfg_read_stable_set_test_hook(void (*hook)(enum aworker_cfg_read_stable_stage stage, const char *path,
							char *buf, size_t len));

/*! Whether the pool is configured (workers > 0) and uses the inline form ('worker N cs7-instance X' for a worker
 * below 'workers'). Only then are views used; otherwise the file is parsed as a whole, as before. */
bool aworker_cfg_model_is_inline(const struct aworker_cfg_model *m);

/*! The worker that owns cs7 instance \a cs7_id through the inline form, or -1. */
int aworker_cfg_model_owner(const struct aworker_cfg_model *m, uint32_t cs7_id);

/*! Check the rules of the inline form and decision D-S1. \a supervisor_mode: the pool is run by the Supervisor,
 * which accepts the inline form only. Fills up to \a max problems into \a out.
 * \returns the number of problems found (which may exceed max); 0 if the configuration is consistent. */
unsigned int aworker_cfg_model_check(const struct aworker_cfg_model *m, bool supervisor_mode,
				     struct aworker_cfg_problem *out, unsigned int max);

/*! The view of \a role (worker_idx for AWORKER_CFG_VIEW_WORKER), as a NUL-terminated talloc string under \a ctx;
 * *len is set to its length. It has exactly as many lines as the file. NULL if the worker has no inline
 * declaration. */
char *aworker_cfg_model_view(void *ctx, const struct aworker_cfg_model *m, enum aworker_cfg_view_role role,
			     unsigned int worker_idx, size_t *len);

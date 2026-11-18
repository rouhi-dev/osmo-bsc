/* bsc-aworker: the A-worker Supervisor, a process separate from osmo-bsc (S2-3) */
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

/* ROLES
 * =====
 *
 * Supervisor (the default): reads osmo-bsc.cfg once (aworker_pool_supervisor_read_config(): the inline form only,
 * D-S1; no 'on-worker-failure shutdown', D-S8), parses only its view of it -- the log targets and 'a-worker-pool' --
 * and runs the A-worker pool: it makes each worker's configuration snapshot, starts the workers by re-executing its
 * own image, restarts them by the pool's policy, and stops them with GOODBYE on SIGTERM / SIGINT. It shares nothing
 * with osmo-bsc and starts no VTY, CTRL or other server (D-S6): it is controlled by signals only. No BSC attaches to
 * it yet (sessions are S2-4 onwards); its workers leave SSN 254 unbound.
 *
 * Worker (internal, not for manual use): a process the Supervisor starts with --aworker-fd N --aworker-index I
 * --aworker-snapshot-fd M (decision B4: the contract of osmo-bsc's inline-form workers, strictly validated), and
 * --aworker-incarnation K, its start number, which only bsc-aworker passes (B5: its lines begin "aworker <idx> inc
 * <K>: "). It reads its snapshot, checks it, parses it and runs its cs7 instance (aworker_run()). These options are
 * not in --help.
 *
 * Log lines (D-S10, B2, B5): the Supervisor keeps the log targets of its view, 'log file' included, and marks its
 * pool lines "aworker-supervisor: ". Workers have no 'log file' (D-2) and log to the stderr they inherit from the
 * Supervisor.
 *
 * Only libaworker and the Osmocom libraries are linked: nothing of osmo-bsc. */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/fsm.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/rate_ctr.h>
#include <osmocom/core/select.h>
#include <osmocom/core/stats.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/xua_msg.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/vty.h>

#include "bscconfig.h"

#include <osmocom/bsc/aworker.h>
#include <osmocom/bsc/aworker_log.h>
#include <osmocom/bsc/aworker_pool.h>

static void *g_ctx;
static const char *g_config_file = "osmo-bsc.cfg";
static bool g_config_given;

/* Worker role (internal): as given, checked in worker_role(). */
static int g_aworker_fd = -1;
static const char *g_aworker_index;
static int g_snapshot_fd = -1;
static bool g_aworker_config;
static const char *g_incarnation;

static const struct log_info log_info = {
	.cat = osmo_bsc_categories,
	.num_cat = ARRAY_SIZE(osmo_bsc_categories),
};

static struct vty_app_info vty_info = {
	.name = "bsc-aworker",
	.version = PACKAGE_VERSION,
	.go_parent_cb = osmo_ss7_vty_go_parent,
};

/* osmo-bsc's 'logging filter imsi IMSI' (a BSC subscriber filter), so that every 'log' section osmo-bsc accepts also
 * parses here (one configuration file, D-S1). The Supervisor and its workers have no subscribers: no effect. */
DEFUN(cfg_log_fltr_imsi, cfg_log_fltr_imsi_cmd,
      "logging filter imsi IMSI",
      LOGGING_STR FILTER_STR
      "Filter log messages by IMSI (osmo-bsc only; no effect in bsc-aworker, which has no subscribers)\n"
      "IMSI to be used as filter\n")
{
	return CMD_SUCCESS;
}

static void print_help(void)
{
	printf("Usage: bsc-aworker [-c FILE]\n");
	printf("The A-worker Supervisor: runs the A-worker pool configured in osmo-bsc.cfg ('a-worker-pool' with\n"
	       "'worker <N> cs7-instance <X>'), as a process of its own. Controlled by signals: SIGTERM or SIGINT stop\n"
	       "the pool and the program.\n\n");
	printf("  -h --help			This text.\n");
	printf("  -V --version			Print the version.\n");
	printf("  -c --config-file filename	The configuration file (osmo-bsc.cfg) to use.\n");
	printf("  -d --debug option		--debug=DRLL:DMM:DRR:DRSL:DNM enable debugging.\n");
	printf("  -s --disable-color		Disable coloring log in stderr.\n");
	printf("  -T --timestamp		Print a timestamp in the debug output.\n");
	printf("  -e --log-level number		Set a global loglevel.\n");
}

static int parse_fd(const char *opt, const char *arg)
{
	char *end = NULL;
	long v;

	errno = 0;
	v = (arg[0] >= '0' && arg[0] <= '9') ? strtol(arg, &end, 10) : -1;
	if (errno || !end || *end || v < 3 || v > INT_MAX) {
		fprintf(stderr, "bsc-aworker: %s needs a file descriptor above 2, not '%s'\n", opt, arg);
		exit(2);
	}
	return (int)v;
}

static void handle_options(int argc, char **argv)
{
	static const struct option long_options[] = {
		{ "help", 0, 0, 'h' },
		{ "version", 0, 0, 'V' },
		{ "config-file", 1, 0, 'c' },
		{ "debug", 1, 0, 'd' },
		{ "disable-color", 0, 0, 's' },
		{ "timestamp", 0, 0, 'T' },
		{ "log-level", 1, 0, 'e' },
		{ "aworker-fd", 1, 0, 1 },
		{ "aworker-index", 1, 0, 2 },
		{ "aworker-snapshot-fd", 1, 0, 3 },
		{ "aworker-config", 1, 0, 4 },
		{ "aworker-incarnation", 1, 0, 5 },
		{ 0, 0, 0, 0 }
	};
	int c;

	while ((c = getopt_long(argc, argv, "hVc:d:sTe:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			print_help();
			exit(0);
		case 'V':
			printf("bsc-aworker version %s\n", PACKAGE_VERSION);
			exit(0);
		case 'c':
			g_config_file = optarg;
			g_config_given = true;
			break;
		case 'd':
			log_parse_category_mask(osmo_stderr_target, optarg);
			break;
		case 's':
			log_set_use_color(osmo_stderr_target, 0);
			break;
		case 'T':
			log_set_print_timestamp(osmo_stderr_target, 1);
			break;
		case 'e':
			log_set_log_level(osmo_stderr_target, atoi(optarg));
			break;
		case 1:
			g_aworker_fd = parse_fd("--aworker-fd", optarg);
			break;
		case 2:
			g_aworker_index = optarg;
			break;
		case 3:
			g_snapshot_fd = parse_fd("--aworker-snapshot-fd", optarg);
			break;
		case 4:
			g_aworker_config = true;
			break;
		case 5:
			g_incarnation = optarg;
			break;
		default:
			fprintf(stderr, "bsc-aworker: invalid option; see 'bsc-aworker --help'\n");
			exit(2);
		}
	}
	if (optind < argc) {
		fprintf(stderr, "bsc-aworker: unexpected argument '%s'; see 'bsc-aworker --help'\n", argv[optind]);
		exit(2);
	}
}

static bool worker_role_requested(void)
{
	return g_aworker_fd >= 0 || g_aworker_index || g_snapshot_fd >= 0 || g_aworker_config || g_incarnation;
}

/* Worker role: check the internal options strictly (B4), then run; never returns. */
static void worker_role(void)
{
	char *end = NULL;
	long idx;

	if (g_aworker_config) {
		fprintf(stderr, "bsc-aworker: a worker of bsc-aworker starts from its configuration snapshot"
			" (--aworker-snapshot-fd); --aworker-config is not supported\n");
		exit(2);
	}
	if (g_aworker_fd < 0 || g_snapshot_fd < 0) {
		fprintf(stderr, "bsc-aworker: the worker role needs --aworker-fd, --aworker-index and"
			" --aworker-snapshot-fd\n");
		exit(2);
	}
	if (g_snapshot_fd == g_aworker_fd) {
		fprintf(stderr, "bsc-aworker: --aworker-snapshot-fd must differ from --aworker-fd (%d)\n", g_aworker_fd);
		exit(2);
	}
	/* Digits only: no sign, no blanks. */
	idx = g_aworker_index && g_aworker_index[0] >= '0' && g_aworker_index[0] <= '9'
	      ? strtol(g_aworker_index, &end, 10) : -1;
	if (!g_aworker_index || !end || *end || idx < 0 || idx >= AWORKER_POOL_MAX_WORKERS) {
		fprintf(stderr, "bsc-aworker: the worker role needs --aworker-index with a worker index from 0 to %d\n",
			AWORKER_POOL_MAX_WORKERS - 1);
		exit(2);
	}
	if (g_config_given) {
		fprintf(stderr, "bsc-aworker: -c / --config-file cannot be combined with the worker role: a worker reads"
			" only its snapshot\n");
		exit(2);
	}
	/* Optional (B5): the start number the Supervisor gave this process, for its log lines. Digits only, 1 or more. */
	if (g_incarnation) {
		long inc;
		end = NULL;
		errno = 0;
		inc = g_incarnation[0] >= '0' && g_incarnation[0] <= '9' ? strtol(g_incarnation, &end, 10) : -1;
		if (errno || !end || *end || inc < 1 || inc > UINT_MAX) {
			fprintf(stderr, "bsc-aworker: --aworker-incarnation needs a number from 1, not '%s'\n",
				g_incarnation);
			exit(2);
		}
		aworker_incarnation = (unsigned int)inc;
	}
	aworker_run(g_ctx, g_aworker_fd, (unsigned int)idx, NULL, g_snapshot_fd);
}

static void signal_handler(int signum)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		/* A second request does not wait for the first one any more. */
		if (osmo_select_shutdown_requested())
			exit(-1);
		osmo_select_shutdown_request();
		break;
	case SIGABRT:
		talloc_report_full(g_ctx, stderr);
		signal(SIGABRT, SIG_DFL);
		raise(SIGABRT);
		break;
	case SIGUSR1:
		talloc_report_full(g_ctx, stderr);
		break;
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	/* Names itself in the A-worker messages and the IPC HELLO (B5). */
	aworker_program_name = "bsc-aworker";
	g_ctx = talloc_named_const(NULL, 1, "bsc-aworker");
	msgb_talloc_ctx_init(g_ctx, 0);
	osmo_xua_msg_tall_ctx_init(g_ctx);
	vty_info.tall_ctx = g_ctx;

	osmo_init_logging2(g_ctx, &log_info);
	osmo_stats_init(g_ctx);
	rate_ctr_init(g_ctx);
	osmo_fsm_set_dealloc_ctx(OTC_SELECT);
	osmo_fsm_log_timeouts(true);

	/* The commands needed to parse the configuration, and no server (D-S6): the log targets as in osmo-bsc,
	 * 'a-worker-pool', and for the worker role the cs7 / SCCP nodes. */
	vty_init(&vty_info);
	logging_vty_add_cmds();
	logging_vty_add_deprecated_subsys(g_ctx, "cc");
	logging_vty_add_deprecated_subsys(g_ctx, "mgcp");
	logging_vty_add_deprecated_subsys(g_ctx, "nat");
	install_element(CFG_LOG_NODE, &cfg_log_fltr_imsi_cmd);
	OSMO_ASSERT(osmo_ss7_init() == 0);
	osmo_ss7_vty_init_asp(g_ctx);
	osmo_sccp_vty_init();
	aworker_pool_vty_init(g_ctx);

	handle_options(argc, argv);
	if (worker_role_requested())
		worker_role();	/* never returns */

	/* Supervisor role: every line of the pool is marked as the Supervisor's (B5). */
	aworker_log_prefix = "aworker-supervisor: ";
	if (aworker_pool_supervisor_read_config(g_config_file) < 0) {
		fprintf(stderr, "bsc-aworker: the configuration is refused. exiting.\n");
		exit(1);
	}

	signal(SIGINT, &signal_handler);
	signal(SIGTERM, &signal_handler);
	signal(SIGABRT, &signal_handler);
	signal(SIGUSR1, &signal_handler);
	/* The IPC links refuse to run unless SIGPIPE is ignored. */
	osmo_init_ignore_signals();

	aworker_pool_set_exec(argv[0], argc, argv);
	if (aworker_pool_start(g_ctx) < 0) {
		fprintf(stderr, "bsc-aworker: cannot start the A-worker pool. exiting.\n");
		exit(1);
	}

	while (!osmo_select_shutdown_done())
		osmo_select_main_ctx(0);

	/* GOODBYE to every worker, then collect them (aworker_pool_stop()). */
	aworker_pool_stop();
	exit(aworker_pool_shutdown_was_fatal() ? 1 : 0);
}

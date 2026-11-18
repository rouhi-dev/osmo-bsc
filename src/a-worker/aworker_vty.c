/* A-worker pool: VTY configuration and introspection */
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

#include <errno.h>
#include <stdlib.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/vty.h>

/* The pool configuration only takes effect at start-up: the workers are forked before the main process opens any
 * socket of its own, and changing the ownership of a running SIGTRAN stack is not possible. Every command here is
 * therefore marked as requiring a restart. */
#define AWORKER_RESTART_NOTE(vty) do { \
	if ((vty)->type != VTY_FILE) \
		vty_out((vty), "%% Changes to 'a-worker-pool' take effect after restarting osmo-bsc%s", \
			VTY_NEWLINE); \
	} while (0)

/* Parent of the strings the configuration commands store (config file names), see aworker_pool_vty_init(). */
static void *g_vty_ctx;

static struct cmd_node aworker_pool_node = {
	AWORKER_POOL_NODE,
	"%s(config-a-worker-pool)# ",
	1,
};

DEFUN(cfg_aworker_pool, cfg_aworker_pool_cmd,
      "a-worker-pool",
      "Configure the pool of A-interface (SIGTRAN) worker processes\n")
{
	vty->node = AWORKER_POOL_NODE;
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_workers, cfg_aworker_workers_cmd,
	   "workers <0-16>",
	   "Number of A-interface worker processes; 0 (the default) runs the SIGTRAN stack inside osmo-bsc\n"
	   "Number of worker processes\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int n = atoi(argv[0]);

	if (n > AWORKER_POOL_MAX_WORKERS) {
		vty_out(vty, "%% At most %u workers are supported%s", AWORKER_POOL_MAX_WORKERS, VTY_NEWLINE);
		return CMD_WARNING;
	}
	cfg->num_workers = n;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_cfg_file, cfg_aworker_cfg_file_cmd,
	   "worker <0-15> config-file FILE",
	   "Configure one worker of the pool\n" "Worker index\n"
	   "Configuration file the worker reads; it must contain the cs7 instance(s) of this worker,"
	   " with their asp / as / route nodes, and nothing else\n"
	   "Path of the worker configuration file\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int idx = atoi(argv[0]);

	if (idx >= AWORKER_POOL_MAX_WORKERS) {
		vty_out(vty, "%% Worker index must be below %u%s", AWORKER_POOL_MAX_WORKERS, VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (cfg->has_cs7_instance[idx]) {
		vty_out(vty, "%% Worker %u already uses the inline form 'worker %u cs7-instance %u'; a worker uses"
			" exactly one form ('no worker %u cs7-instance' first)%s", idx, idx, cfg->cs7_instance[idx], idx,
			VTY_NEWLINE);
		return CMD_WARNING;
	}
	osmo_talloc_replace_string(g_vty_ctx, &cfg->cfg_file[idx], argv[1]);
	if (!cfg->cfg_file[idx])
		return CMD_WARNING;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_no_cfg_file, cfg_aworker_no_cfg_file_cmd,
	   "no worker <0-15> config-file",
	   NO_STR "Remove a worker's configuration file\n" "Worker index\n" "Configuration file\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int idx = atoi(argv[0]);

	if (idx >= AWORKER_POOL_MAX_WORKERS) {
		vty_out(vty, "%% Worker index must be below %u%s", AWORKER_POOL_MAX_WORKERS, VTY_NEWLINE);
		return CMD_WARNING;
	}
	TALLOC_FREE(cfg->cfg_file[idx]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_startup_timeout, cfg_aworker_startup_timeout_cmd,
	   "startup-timeout <1-600>",
	   "How long each started worker (at osmo-bsc start, restart or reset) has to report its cs7 instances;"
	   " a worker that does not is treated as lost (see 'on-worker-failure')\n"
	   "Timeout in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->startup_timeout = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_restart_delay, cfg_aworker_restart_delay_cmd,
	   "restart-delay <0-600>",
	   "How long to wait before restarting a worker after its first unplanned loss ('on-worker-failure"
	   " restart'); every further loss in a row doubles the delay, up to 'restart-delay-max'\n"
	   "Delay in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->restart_delay = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_restart_delay_max, cfg_aworker_restart_delay_max_cmd,
	   "restart-delay-max <1-3600>",
	   "Longest restart delay the exponential backoff reaches (never less than 'restart-delay')\n"
	   "Delay in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->restart_delay_max = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_stable_time, cfg_aworker_stable_time_cmd,
	   "stable-time <1-3600>",
	   "How long a worker must have been READY before it was lost for its restart delay to start over at"
	   " 'restart-delay'\n"
	   "Time in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->stable_time = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_max_restarts, cfg_aworker_max_restarts_cmd,
	   "max-restarts <1-32> window <1-86400>",
	   "Stop restarting a worker that keeps being lost: an unplanned loss that would be the N-th restart within"
	   " the window (counted from the oldest restart recorded in it) makes the worker FAILED instead, so at"
	   " most N-1 restarts happen within any window; 'a-worker-pool reset worker' starts it again\n"
	   "N; 1: no automatic restart at all\n"
	   "The window\n"
	   "Window in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->max_restarts = atoi(argv[0]);
	aworker_pool_cfg()->restart_window = atoi(argv[1]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_on_failure, cfg_aworker_on_failure_cmd,
	   "on-worker-failure (restart|shutdown)",
	   "What to do when a worker process dies or its IPC link breaks. In both cases every SCCP connection of"
	   " the worker's cs7 instances is terminated and the BSSMAP RESET procedure is restarted; no SCCP"
	   " operation is ever replayed.\n"
	   "Restart the worker after its restart delay (the default; see 'restart-delay')\n"
	   "Terminate osmo-bsc\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->on_failure = !strcmp(argv[0], "shutdown") ? AWORKER_FAILURE_SHUTDOWN
								     : AWORKER_FAILURE_RESTART;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_cs7_instance, cfg_aworker_cs7_instance_cmd,
	   "worker <0-15> cs7-instance <0-15>",
	   "Configure one worker of the pool\n" "Worker index\n"
	   "The worker owns this cs7 instance of this configuration file (inline form): its asp / as / listen nodes"
	   " are parsed by that worker only, never by the BSC\n"
	   "cs7 instance number\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int idx = atoi(argv[0]);
	unsigned int cs7 = atoi(argv[1]);

	if (idx >= AWORKER_POOL_MAX_WORKERS) {
		vty_out(vty, "%% Worker index must be below %u%s", AWORKER_POOL_MAX_WORKERS, VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (cfg->cfg_file[idx]) {
		vty_out(vty, "%% Worker %u already uses 'worker %u config-file %s'; a worker uses exactly one form"
			" ('no worker %u config-file' first)%s", idx, idx, cfg->cfg_file[idx], idx, VTY_NEWLINE);
		return CMD_WARNING;
	}
	cfg->has_cs7_instance[idx] = true;
	cfg->cs7_instance[idx] = cs7;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_no_cs7_instance, cfg_aworker_no_cs7_instance_cmd,
	   "no worker <0-15> cs7-instance",
	   NO_STR "Remove a worker's inline cs7 instance\n" "Worker index\n" "Inline cs7 instance\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int idx = atoi(argv[0]);

	if (idx >= AWORKER_POOL_MAX_WORKERS) {
		vty_out(vty, "%% Worker index must be below %u%s", AWORKER_POOL_MAX_WORKERS, VTY_NEWLINE);
		return CMD_WARNING;
	}
	cfg->has_cs7_instance[idx] = false;
	cfg->cs7_instance[idx] = 0;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_supervisor_socket, cfg_aworker_supervisor_socket_cmd,
	   "supervisor-socket PATH",
	   "The Supervisor's (bsc-aworker's) session socket: an AF_UNIX path, absolute; its directory must exist, belong to"
	   " the Supervisor's user and be writable by nobody else. Without it the Supervisor accepts no BSC session\n"
	   "Absolute path of the socket\n",
	   CMD_ATTR_IMMEDIATE)
{
	struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	const char *path = argv[0];

	/* N5: a path in the file system only (no abstract namespace), and one that fits sun_path with its NUL. */
	if (path[0] != '/') {
		vty_out(vty, "%% supervisor-socket must be an absolute path%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (strlen(path) >= AWORKER_SOCKET_PATH_MAX) {
		vty_out(vty, "%% supervisor-socket is too long (at most %u characters)%s", AWORKER_SOCKET_PATH_MAX - 1,
			VTY_NEWLINE);
		return CMD_WARNING;
	}
	osmo_talloc_replace_string(g_vty_ctx, &cfg->supervisor_socket, path);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_no_supervisor_socket, cfg_aworker_no_supervisor_socket_cmd,
	   "no supervisor-socket",
	   NO_STR "No session socket: the Supervisor accepts no BSC session (the default)\n",
	   CMD_ATTR_IMMEDIATE)
{
	TALLOC_FREE(aworker_pool_cfg()->supervisor_socket);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_session_echo_interval, cfg_aworker_session_echo_interval_cmd,
	   "session-echo-interval <1-3600>",
	   "Seconds between the Supervisor's ECHO requests to an attached BSC (session health; never affects the"
	   " workers)\n"
	   "Seconds (default 2)\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->session_echo_interval = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_session_echo_misses, cfg_aworker_session_echo_misses_cmd,
	   "session-echo-misses <1-100>",
	   "Consecutive unanswered session ECHO requests that are reported as the threshold (the session is UNRESPONSIVE"
	   " from the first; nothing is done to the workers)\n"
	   "Number (default 3)\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->session_echo_misses = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

int aworker_pool_vty_config_is_consistent(struct vty *vty)
{
	const char *refusal = aworker_pool_config_write_refusal();

	if (!refusal)
		return 1;
	vty_out(vty, "%% %s%s", refusal, VTY_NEWLINE);
	return 0;
}

static int config_write_aworker_pool(struct vty *vty)
{
	const struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int i;
	bool any = cfg->num_workers != 0
		   || cfg->startup_timeout != AWORKER_STARTUP_TIMEOUT_DEFAULT
		   || cfg->restart_delay != AWORKER_RESTART_DELAY_DEFAULT
		   || cfg->restart_delay_max != AWORKER_RESTART_DELAY_MAX_DEFAULT
		   || cfg->stable_time != AWORKER_STABLE_TIME_DEFAULT
		   || cfg->max_restarts != AWORKER_MAX_RESTARTS_DEFAULT
		   || cfg->restart_window != AWORKER_RESTART_WINDOW_DEFAULT
		   || cfg->on_failure != AWORKER_FAILURE_RESTART
		   || cfg->supervisor_socket
		   || cfg->session_echo_interval != AWORKER_SESSION_ECHO_INTERVAL_DEFAULT
		   || cfg->session_echo_misses != AWORKER_SESSION_ECHO_MISSES_DEFAULT;

	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (cfg->cfg_file[i] || cfg->has_cs7_instance[i])
			any = true;
	}
	if (!any)
		return CMD_SUCCESS;

	vty_out(vty, "a-worker-pool%s", VTY_NEWLINE);
	vty_out(vty, " workers %u%s", cfg->num_workers, VTY_NEWLINE);
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (cfg->cfg_file[i])
			vty_out(vty, " worker %u config-file %s%s", i, cfg->cfg_file[i], VTY_NEWLINE);
		if (cfg->has_cs7_instance[i])
			vty_out(vty, " worker %u cs7-instance %u%s", i, cfg->cs7_instance[i], VTY_NEWLINE);
	}
	if (cfg->startup_timeout != AWORKER_STARTUP_TIMEOUT_DEFAULT)
		vty_out(vty, " startup-timeout %u%s", cfg->startup_timeout, VTY_NEWLINE);
	if (cfg->restart_delay != AWORKER_RESTART_DELAY_DEFAULT)
		vty_out(vty, " restart-delay %u%s", cfg->restart_delay, VTY_NEWLINE);
	if (cfg->restart_delay_max != AWORKER_RESTART_DELAY_MAX_DEFAULT)
		vty_out(vty, " restart-delay-max %u%s", cfg->restart_delay_max, VTY_NEWLINE);
	if (cfg->stable_time != AWORKER_STABLE_TIME_DEFAULT)
		vty_out(vty, " stable-time %u%s", cfg->stable_time, VTY_NEWLINE);
	if (cfg->max_restarts != AWORKER_MAX_RESTARTS_DEFAULT || cfg->restart_window != AWORKER_RESTART_WINDOW_DEFAULT)
		vty_out(vty, " max-restarts %u window %u%s", cfg->max_restarts, cfg->restart_window, VTY_NEWLINE);
	if (cfg->on_failure != AWORKER_FAILURE_RESTART)
		vty_out(vty, " on-worker-failure %s%s",
			get_value_string(aworker_failure_policy_names, cfg->on_failure), VTY_NEWLINE);
	if (cfg->supervisor_socket)
		vty_out(vty, " supervisor-socket %s%s", cfg->supervisor_socket, VTY_NEWLINE);
	if (cfg->session_echo_interval != AWORKER_SESSION_ECHO_INTERVAL_DEFAULT)
		vty_out(vty, " session-echo-interval %u%s", cfg->session_echo_interval, VTY_NEWLINE);
	if (cfg->session_echo_misses != AWORKER_SESSION_ECHO_MISSES_DEFAULT)
		vty_out(vty, " session-echo-misses %u%s", cfg->session_echo_misses, VTY_NEWLINE);
	return CMD_SUCCESS;
}

DEFUN(show_aworker_pool, show_aworker_pool_cmd,
      "show a-worker-pool",
      SHOW_STR "Display the state of the A-interface worker pool\n")
{
	aworker_pool_dump_vty(vty);
	return CMD_SUCCESS;
}

DEFUN(aworker_reset_worker, aworker_reset_worker_cmd,
      "a-worker-pool reset worker <0-15>",
      "A-interface worker pool\n"
      "Leave the FAILED state: forget the failure and start the worker again\n"
      "One worker\n"
      "Worker index\n")
{
	unsigned int idx = atoi(argv[0]);
	int rc = aworker_pool_reset_worker(idx);

	switch (rc) {
	case 0:
		vty_out(vty, "worker %u reset; starting it again%s", idx, VTY_NEWLINE);
		return CMD_SUCCESS;
	case -EBUSY:
		vty_out(vty, "%% worker %u is not FAILED; nothing to reset%s", idx, VTY_NEWLINE);
		return CMD_WARNING;
	case -EINVAL:
		vty_out(vty, "%% there is no worker %u%s", idx, VTY_NEWLINE);
		return CMD_WARNING;
	default:
		vty_out(vty, "%% the A-worker pool is not running%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
}

void aworker_pool_vty_init(void *ctx)
{
	g_vty_ctx = ctx;
	install_element_ve(&show_aworker_pool_cmd);
	install_element(ENABLE_NODE, &aworker_reset_worker_cmd);

	install_element(CONFIG_NODE, &cfg_aworker_pool_cmd);
	install_node(&aworker_pool_node, config_write_aworker_pool);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_workers_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_cfg_file_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_no_cfg_file_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_cs7_instance_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_no_cs7_instance_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_startup_timeout_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_restart_delay_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_restart_delay_max_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_stable_time_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_max_restarts_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_on_failure_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_supervisor_socket_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_no_supervisor_socket_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_session_echo_interval_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_session_echo_misses_cmd);
}

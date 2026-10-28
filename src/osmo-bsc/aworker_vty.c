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

#include <stdlib.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>
#include <osmocom/bsc/vty.h>

/* The pool configuration only takes effect at start-up: the workers are forked before the main process opens any
 * socket of its own, and changing the ownership of a running SIGTRAN stack is not possible. Every command here is
 * therefore marked as requiring a restart. */
#define AWORKER_RESTART_NOTE(vty) do { \
	if ((vty)->type != VTY_FILE) \
		vty_out((vty), "%% Changes to 'a-worker-pool' take effect after restarting osmo-bsc%s", \
			VTY_NEWLINE); \
	} while (0)

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
	osmo_talloc_replace_string(tall_bsc_ctx, &cfg->cfg_file[idx], argv[1]);
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
	   "How long to wait for the workers to report their cs7 instances before logging an error\n"
	   "Timeout in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->startup_timeout = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_restart_delay, cfg_aworker_restart_delay_cmd,
	   "restart-delay <0-600>",
	   "How long to wait before restarting a worker that died ('on-worker-failure restart')\n"
	   "Delay in seconds\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->restart_delay = atoi(argv[0]);
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

DEFUN_ATTR(cfg_aworker_on_failure, cfg_aworker_on_failure_cmd,
	   "on-worker-failure (restart|shutdown)",
	   "What to do when a worker process dies or its IPC link breaks. In both cases every SCCP connection of"
	   " the worker's cs7 instances is terminated and the BSSMAP RESET procedure is restarted; no SCCP"
	   " operation is ever replayed.\n"
	   "Restart the worker after 'restart-delay' seconds (the default)\n"
	   "Terminate osmo-bsc\n",
	   CMD_ATTR_IMMEDIATE)
{
	aworker_pool_cfg()->on_failure = !strcmp(argv[0], "shutdown") ? AWORKER_FAILURE_SHUTDOWN
								     : AWORKER_FAILURE_RESTART;
	AWORKER_RESTART_NOTE(vty);
	return CMD_SUCCESS;
}

static int config_write_aworker_pool(struct vty *vty)
{
	const struct aworker_pool_cfg *cfg = aworker_pool_cfg();
	unsigned int i;
	bool any = cfg->num_workers != 0
		   || cfg->startup_timeout != AWORKER_STARTUP_TIMEOUT_DEFAULT
		   || cfg->restart_delay != AWORKER_RESTART_DELAY_DEFAULT
		   || cfg->on_failure != AWORKER_FAILURE_RESTART;

	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (cfg->cfg_file[i])
			any = true;
	}
	if (!any)
		return CMD_SUCCESS;

	vty_out(vty, "a-worker-pool%s", VTY_NEWLINE);
	vty_out(vty, " workers %u%s", cfg->num_workers, VTY_NEWLINE);
	for (i = 0; i < AWORKER_POOL_MAX_WORKERS; i++) {
		if (cfg->cfg_file[i])
			vty_out(vty, " worker %u config-file %s%s", i, cfg->cfg_file[i], VTY_NEWLINE);
	}
	if (cfg->startup_timeout != AWORKER_STARTUP_TIMEOUT_DEFAULT)
		vty_out(vty, " startup-timeout %u%s", cfg->startup_timeout, VTY_NEWLINE);
	if (cfg->restart_delay != AWORKER_RESTART_DELAY_DEFAULT)
		vty_out(vty, " restart-delay %u%s", cfg->restart_delay, VTY_NEWLINE);
	if (cfg->on_failure != AWORKER_FAILURE_RESTART)
		vty_out(vty, " on-worker-failure %s%s",
			get_value_string(aworker_failure_policy_names, cfg->on_failure), VTY_NEWLINE);
	return CMD_SUCCESS;
}

DEFUN(show_aworker_pool, show_aworker_pool_cmd,
      "show a-worker-pool",
      SHOW_STR "Display the state of the A-interface worker pool\n")
{
	aworker_pool_dump_vty(vty);
	return CMD_SUCCESS;
}

void aworker_pool_vty_init(void)
{
	install_element_ve(&show_aworker_pool_cmd);

	install_element(CONFIG_NODE, &cfg_aworker_pool_cmd);
	install_node(&aworker_pool_node, config_write_aworker_pool);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_workers_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_cfg_file_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_no_cfg_file_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_startup_timeout_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_restart_delay_cmd);
	install_element(AWORKER_POOL_NODE, &cfg_aworker_on_failure_cmd);
}

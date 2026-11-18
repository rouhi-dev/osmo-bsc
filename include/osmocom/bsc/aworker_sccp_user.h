/* A-worker pool: osmo-bsc's SCCP users bound through a worker (BSC side only; split from aworker_pool.h, N4) */
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

/* The SCCP-user API of the A-worker pool: osmo-bsc binds its SCCP users (A interface, Lb, ...) in the worker that owns
 * the cs7 instance. It is BSC-specific (bsc_sccp_backend.h); the pool's lifecycle API in aworker_pool.h is not, so that
 * a program without the BSC (bsc-aworker) can use the pool without this header. */

#include <stdbool.h>
#include <stdint.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/bsc_sccp_backend.h>

struct osmo_ss7_instance;
struct bsc_sccp_inst;

/* SCCP Users bound through a worker */

struct aworker_sccp_user_ops {
	/*! The worker could not execute a TX_* request. See the TX_FAILED contract in aworker_ipc.h: an operation
	 * with disposition EXECUTED_ERROR may or may not have reached the network and must not be replayed. */
	void (*tx_failed)(struct bsc_sccp_user *user, const struct aworker_ipc_tx_failed *f);
	/*! The user can no longer be used: the worker is gone or the bind failed. All SCCP connections of this
	 * user must be considered lost. Called at most once per 'available'. */
	void (*unavailable)(struct bsc_sccp_user *user);
	/*! The user is bound in its worker and can be used. */
	void (*available)(struct bsc_sccp_user *user);
};

/*! Bind an SCCP user in the worker that owns cs7 instance \a ss7. The bind is carried out as soon as that worker
 * is known and ready; until then, and whenever the worker is lost, transmit operations on the returned user fail
 * with -ENOTCONN and aworker_sccp_user_ready() is false.
 * \param[in] ctx talloc context of the returned user.
 * \param[in] ss7 cs7 instance (owned by a worker) this user belongs to.
 * \param[in] bsc_sccp the conn_id registry of that cs7 instance, kept by the main process.
 * \returns the new user, or NULL on allocation failure or if worker mode is not running. */
struct bsc_sccp_user *aworker_sccp_user_bind(void *ctx, struct osmo_ss7_instance *ss7,
					     struct bsc_sccp_inst *bsc_sccp, const char *name,
					     bsc_sccp_user_prim_cb prim_cb, uint16_t ssn,
					     const struct aworker_sccp_user_ops *ops);
/*! Find a user bound with aworker_sccp_user_bind() on this cs7 instance and SSN, or NULL. */
struct bsc_sccp_user *aworker_sccp_user_find(uint32_t cs7_id, uint16_t ssn);
void aworker_sccp_user_unbind(struct bsc_sccp_user *user);
bool aworker_sccp_user_ready(const struct bsc_sccp_user *user);

/*! The worker-backed SCCP User backend. */
extern const struct bsc_sccp_backend_ops bsc_sccp_backend_worker_ops;

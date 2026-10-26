/* SCCP User backend for OsmoBSC: the single point through which osmo-bsc talks to its SCCP users */
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

#include <stdint.h>

#include <osmocom/core/prim.h>
#include <osmocom/sigtran/sccp_sap.h>

struct msgb;
struct bsc_sccp_inst;
struct bsc_sccp_user;

/*! Up-call for primitives received from the SCCP stack (N-CONNECT.ind, N-DATA.ind, ...).
 * Same contract as the osmo_prim_cb passed to osmo_sccp_user_bind(): ownership of oph->msg is transferred to the
 * callee, which must free it. */
typedef int (*bsc_sccp_user_prim_cb)(struct bsc_sccp_user *user, struct osmo_prim_hdr *oph);

/*! Operations of an SCCP User backend. Each tx operation has exactly the arguments, return value and msgb ownership
 * semantics of the libosmo-sigtran helper of the same name (osmo_sccp_tx_*() in osmocom/sigtran/sccp_helpers.h). */
struct bsc_sccp_backend_ops {
	const char *name;
	int (*tx_unitdata_msg)(struct bsc_sccp_user *user,
			       const struct osmo_sccp_addr *calling_addr,
			       const struct osmo_sccp_addr *called_addr,
			       struct msgb *msg);
	int (*tx_conn_req_msg)(struct bsc_sccp_user *user, uint32_t conn_id,
			       const struct osmo_sccp_addr *calling_addr,
			       const struct osmo_sccp_addr *called_addr,
			       struct msgb *msg);
	int (*tx_conn_resp)(struct bsc_sccp_user *user, uint32_t conn_id,
			    const struct osmo_sccp_addr *resp_addr,
			    const uint8_t *data, unsigned int len);
	int (*tx_data_msg)(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg);
	int (*tx_disconn)(struct bsc_sccp_user *user, uint32_t conn_id,
			  const struct osmo_sccp_addr *resp_addr, uint32_t cause);
};

/*! An SCCP User as seen by osmo-bsc (one per SCCP instance and local SSN, shared by all MSCs on that instance). */
struct bsc_sccp_user {
	const struct bsc_sccp_backend_ops *ops;
	bsc_sccp_user_prim_cb prim_cb;
	/*! In-process backend: the libosmo-sigtran SCCP User this wraps. */
	struct osmo_sccp_user *scu;
};

/*! The in-process backend: every operation calls the corresponding libosmo-sigtran helper unchanged. */
extern const struct bsc_sccp_backend_ops bsc_sccp_backend_local_ops;

struct bsc_sccp_user *bsc_sccp_user_bind(void *ctx, struct osmo_sccp_instance *sccp, const char *name,
					 bsc_sccp_user_prim_cb prim_cb, uint16_t ssn);
struct bsc_sccp_user *bsc_sccp_user_find(struct osmo_sccp_instance *sccp, uint16_t ssn, uint32_t pc);
void bsc_sccp_user_unbind(struct bsc_sccp_user *user);

struct osmo_sccp_instance *bsc_sccp_user_get_sccp(const struct bsc_sccp_user *user);
struct bsc_sccp_inst *bsc_sccp_user_get_bsc_sccp(const struct bsc_sccp_user *user);

int bsc_sccp_tx_unitdata_msg(struct bsc_sccp_user *user,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg);
int bsc_sccp_tx_conn_req_msg(struct bsc_sccp_user *user, uint32_t conn_id,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg);
int bsc_sccp_tx_conn_resp(struct bsc_sccp_user *user, uint32_t conn_id,
			  const struct osmo_sccp_addr *resp_addr,
			  const uint8_t *data, unsigned int len);
int bsc_sccp_tx_data_msg(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg);
int bsc_sccp_tx_disconn(struct bsc_sccp_user *user, uint32_t conn_id,
			const struct osmo_sccp_addr *resp_addr, uint32_t cause);

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

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/sccp_helpers.h>

#include <osmocom/bsc/bsc_sccp_backend.h>

/* In-process backend: each operation is a plain call of the libosmo-sigtran helper with identical arguments, so that
 * argument handling, validation, return values and msgb ownership are exactly those of the library. */

static int local_tx_unitdata_msg(struct bsc_sccp_user *user,
				 const struct osmo_sccp_addr *calling_addr,
				 const struct osmo_sccp_addr *called_addr,
				 struct msgb *msg)
{
	return osmo_sccp_tx_unitdata_msg(user->scu, calling_addr, called_addr, msg);
}

static int local_tx_conn_req_msg(struct bsc_sccp_user *user, uint32_t conn_id,
				 const struct osmo_sccp_addr *calling_addr,
				 const struct osmo_sccp_addr *called_addr,
				 struct msgb *msg)
{
	return osmo_sccp_tx_conn_req_msg(user->scu, conn_id, calling_addr, called_addr, msg);
}

static int local_tx_conn_resp(struct bsc_sccp_user *user, uint32_t conn_id,
			      const struct osmo_sccp_addr *resp_addr,
			      const uint8_t *data, unsigned int len)
{
	return osmo_sccp_tx_conn_resp(user->scu, conn_id, resp_addr, data, len);
}

static int local_tx_data_msg(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg)
{
	return osmo_sccp_tx_data_msg(user->scu, conn_id, msg);
}

static int local_tx_disconn(struct bsc_sccp_user *user, uint32_t conn_id,
			    const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	return osmo_sccp_tx_disconn(user->scu, conn_id, resp_addr, cause);
}

const struct bsc_sccp_backend_ops bsc_sccp_backend_local_ops = {
	.name = "local",
	.tx_unitdata_msg = local_tx_unitdata_msg,
	.tx_conn_req_msg = local_tx_conn_req_msg,
	.tx_conn_resp = local_tx_conn_resp,
	.tx_data_msg = local_tx_data_msg,
	.tx_disconn = local_tx_disconn,
};

/* osmo_prim_cb installed in libosmo-sigtran: hand the primitive, and with it ownership of oph->msg, to the
 * bsc_sccp_user's up-call. */
static int local_sap_up(struct osmo_prim_hdr *oph, void *_scu)
{
	struct osmo_sccp_user *scu = _scu;
	struct bsc_sccp_user *user = osmo_sccp_user_get_priv(scu);

	OSMO_ASSERT(user);
	return user->prim_cb(user, oph);
}

/*! Bind a new SCCP User on the given SCCP instance and SSN, like osmo_sccp_user_bind().
 * \param[in] ctx  talloc context for the returned bsc_sccp_user.
 * \returns the new user, or NULL if osmo_sccp_user_bind() failed. */
struct bsc_sccp_user *bsc_sccp_user_bind(void *ctx, struct osmo_sccp_instance *sccp, const char *name,
					 bsc_sccp_user_prim_cb prim_cb, uint16_t ssn)
{
	struct bsc_sccp_user *user;
	struct osmo_sccp_user *scu;

	OSMO_ASSERT(prim_cb);

	scu = osmo_sccp_user_bind(sccp, name, local_sap_up, ssn);
	if (!scu)
		return NULL;

	user = talloc_zero(ctx, struct bsc_sccp_user);
	OSMO_ASSERT(user);
	*user = (struct bsc_sccp_user){
		.ops = &bsc_sccp_backend_local_ops,
		.prim_cb = prim_cb,
		.scu = scu,
	};
	osmo_sccp_user_set_priv(scu, user);
	return user;
}

/*! Find a user previously bound with bsc_sccp_user_bind(), like osmo_sccp_user_find().
 * \returns the user, or NULL if there is no SCCP User for this SSN and PC. */
struct bsc_sccp_user *bsc_sccp_user_find(struct osmo_sccp_instance *sccp, uint16_t ssn, uint32_t pc)
{
	struct osmo_sccp_user *scu = osmo_sccp_user_find(sccp, ssn, pc);
	struct bsc_sccp_user *user;

	if (!scu)
		return NULL;
	/* osmo-bsc binds all of its SCCP Users through bsc_sccp_user_bind(), so a user found here must carry its
	 * bsc_sccp_user. Anything else would mean a second, unwrapped user on our SSN. */
	user = osmo_sccp_user_get_priv(scu);
	OSMO_ASSERT(user);
	return user;
}

/*! Unbind and free a user, like osmo_sccp_user_unbind(). */
void bsc_sccp_user_unbind(struct bsc_sccp_user *user)
{
	if (!user)
		return;
	osmo_sccp_user_unbind(user->scu);
	talloc_free(user);
}

/*! The SCCP instance the user is bound on, or NULL if the backend has none (worker backend). */
struct osmo_sccp_instance *bsc_sccp_user_get_sccp(const struct bsc_sccp_user *user)
{
	if (!user->scu)
		return NULL;
	return osmo_sccp_get_sccp(user->scu);
}

/*! The cs7 instance of the user: the one of its SCCP instance in the in-process backend, and the one the user
 * was bound on in the worker backend (where this process has no SCCP instance). */
struct osmo_ss7_instance *bsc_sccp_user_get_ss7(const struct bsc_sccp_user *user)
{
	struct osmo_sccp_instance *sccp;

	if (user->ss7)
		return user->ss7;
	sccp = bsc_sccp_user_get_sccp(user);
	return sccp ? osmo_sccp_get_ss7(sccp) : NULL;
}

/*! The osmo-bsc conn_id registry of the user's cs7 instance.
 * Like the code this replaces, the in-process backend looks up the SCCP instance's priv at the time of the call. */
struct bsc_sccp_inst *bsc_sccp_user_get_bsc_sccp(const struct bsc_sccp_user *user)
{
	if (user->bsc_sccp)
		return user->bsc_sccp;
	return osmo_sccp_get_priv(bsc_sccp_user_get_sccp(user));
}

int bsc_sccp_tx_unitdata_msg(struct bsc_sccp_user *user,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	return user->ops->tx_unitdata_msg(user, calling_addr, called_addr, msg);
}

int bsc_sccp_tx_conn_req_msg(struct bsc_sccp_user *user, uint32_t conn_id,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	return user->ops->tx_conn_req_msg(user, conn_id, calling_addr, called_addr, msg);
}

int bsc_sccp_tx_conn_resp(struct bsc_sccp_user *user, uint32_t conn_id,
			  const struct osmo_sccp_addr *resp_addr,
			  const uint8_t *data, unsigned int len)
{
	return user->ops->tx_conn_resp(user, conn_id, resp_addr, data, len);
}

int bsc_sccp_tx_data_msg(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg)
{
	return user->ops->tx_data_msg(user, conn_id, msg);
}

int bsc_sccp_tx_disconn(struct bsc_sccp_user *user, uint32_t conn_id,
			const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	return user->ops->tx_disconn(user, conn_id, resp_addr, cause);
}

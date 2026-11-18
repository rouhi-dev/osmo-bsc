/* A-worker pool: the osmo-bsc SCCP user side (users bound in the workers, the SCCP backend towards them) */
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
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/aworker_sccp_user.h>
#include <osmocom/bsc/aworker_pool_internal.h>
#include <osmocom/bsc/bsc_sccp_backend.h>
#include <osmocom/bsc/debug.h>

/*! One SCCP User the main process keeps in a worker. */
struct aworker_user {
	struct llist_head entry;		/*!< in aworker_pool.users */
	struct aworker_pool *pool;
	uint32_t cs7_id;
	uint16_t ssn;
	char name[AWORKER_IPC_MAX_USER_NAME + 1];
	const struct aworker_sccp_user_ops *ops;
	struct bsc_sccp_user *user;		/*!< the generic user; owns nothing of this struct */

	/*! Worker this user is currently bound in, and the user_ref valid in it. Both are reset whenever the
	 * worker is lost, and a fresh user_ref is drawn on the next bind, so that no record of a previous worker
	 * (or of a previous bind) can ever be attributed to this user. */
	struct aworker *worker;
	uint32_t user_ref;
	bool bind_sent;
	bool bound;
};

#define LOGU(au, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker user '%s' (cs7 %u ssn %u): " fmt, (au)->name, (au)->cs7_id, (au)->ssn, ## args)

/* A user of this worker by the user_ref we assigned to it. Only refs of the current bind match. */
static struct aworker_user *user_by_ref(struct aworker *w, uint32_t user_ref)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &w->pool->users, entry) {
		if (au->worker == w && au->bind_sent && au->user_ref == user_ref)
			return au;
	}
	return NULL;
}

static struct aworker_user *user_by_cs7_ssn(struct aworker_pool *pool, uint32_t cs7_id, uint16_t ssn)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry) {
		if (au->cs7_id == cs7_id && au->ssn == ssn)
			return au;
	}
	return NULL;
}

/***********************************************************************
 * User state transitions
 ***********************************************************************/

static void user_set_unavailable(struct aworker_user *au)
{
	bool was_bound = au->bound;

	au->worker = NULL;
	au->bind_sent = false;
	au->bound = false;
	if (was_bound && au->ops && au->ops->unavailable)
		au->ops->unavailable(au->user);
}

/* Queue a USER_BIND for this user in its worker. */
static void user_try_bind(struct aworker_user *au)
{
	struct aworker_pool *pool = au->pool;
	struct aworker *w;
	struct msgb *rec;
	uint32_t seq;
	int rc;

	if (au->bind_sent || pool->stopping)
		return;
	w = worker_by_cs7(pool, au->cs7_id);
	if (!w)
		return;

	/* A fresh user_ref per bind: records of an earlier bind or of a dead worker can never match. */
	au->user_ref = pool->next_user_ref++;
	if (!pool->next_user_ref)
		pool->next_user_ref = 1;
	au->worker = w;

	rc = aworker_ipc_enc_user_bind(pool->ctx, &rec, au->user_ref, au->cs7_id, au->ssn, au->name);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode USER_BIND: %s\n", strerror(-rc));
		au->worker = NULL;
		return;
	}
	rc = aworker_ipc_link_send(w->link, rec, &seq);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot send USER_BIND to worker %u: %s\n", w->idx, strerror(-rc));
		au->worker = NULL;
		return;
	}
	au->bind_sent = true;
	LOGU(au, LOGL_INFO, "USER_BIND sent to worker %u (user_ref %u, seq %u)\n", w->idx, au->user_ref, seq);
}

static void users_try_bind_all(struct aworker_pool *pool)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry)
		user_try_bind(au);
}

/* Every user that was bound in, or waiting for, this worker loses it. */
static void users_lose_worker(struct aworker *w)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &w->pool->users, entry) {
		if (au->worker == w)
			user_set_unavailable(au);
	}
}

static void handle_bind_result(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_user *au = user_by_ref(w, m->user_ref);

	if (!au) {
		LOGW(w, LOGL_NOTICE, "USER_BIND_RESULT for unknown user_ref %u, ignoring\n", m->user_ref);
		return;
	}
	if (m->u.bind_result != AWORKER_IPC_BIND_OK) {
		LOGU(au, LOGL_ERROR, "worker %u refused USER_BIND: result %u\n", w->idx, m->u.bind_result);
		/* Leave bind_sent set: the worker told us this cs7/ssn combination does not work, so do not spin
		 * on it. It is retried when the worker is restarted. */
		au->bound = false;
		return;
	}
	if (au->bound) {
		LOGU(au, LOGL_NOTICE, "duplicate USER_BIND_RESULT, ignoring\n");
		return;
	}
	au->bound = true;
	LOGU(au, LOGL_NOTICE, "bound in worker %u\n", w->idx);
	if (au->ops && au->ops->available)
		au->ops->available(au->user);
}

static void handle_scu_up(struct aworker *w, const struct aworker_ipc_msg *m)
{
	struct aworker_user *au = user_by_ref(w, m->user_ref);
	struct osmo_scu_prim *prim;
	struct msgb *upmsg;

	if (!au) {
		/* Can legitimately race an unbind, or be a record of a user we gave up on. */
		LOGW(w, LOGL_NOTICE, "SCU_UP for unknown user_ref %u, dropping\n", m->user_ref);
		return;
	}
	if (!au->bound) {
		LOGU(au, LOGL_NOTICE, "SCU_UP before USER_BIND_RESULT, dropping\n");
		return;
	}
	upmsg = aworker_ipc_build_scu_prim(au->pool->ctx, m);
	if (!upmsg) {
		LOGU(au, LOGL_ERROR, "cannot allocate SCCP primitive, dropping SCU_UP\n");
		return;
	}
	prim = (struct osmo_scu_prim *)msgb_data(upmsg);
	/* Same contract as the in-process backend: the up-call owns the msgb. */
	au->user->prim_cb(au->user, &prim->oph);
}

static void handle_tx_failed(struct aworker *w, const struct aworker_ipc_msg *m)
{
	const struct aworker_ipc_tx_failed *f = &m->u.tx_failed;
	struct aworker_user *au = user_by_ref(w, m->user_ref);

	if (!au) {
		LOGW(w, LOGL_NOTICE, "TX_FAILED for unknown user_ref %u, ignoring\n", m->user_ref);
		return;
	}
	LOGU(au, f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? LOGL_ERROR : LOGL_FATAL,
	     "worker %u could not send %s (seq %u, conn_id %u): %s, code %u, lib_rc %d%s\n",
	     w->idx, aworker_ipc_msg_type_name(f->ref_type), f->ref_seq, f->conn_id,
	     f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? "not executed" : "executed with an error",
	     f->code, f->lib_rc,
	     f->disposition == AWORKER_IPC_TXF_NOT_EXECUTED ? "" :
	     " -- the operation may or may not have reached the network and is NOT retried");
	if (au->ops && au->ops->tx_failed)
		au->ops->tx_failed(au->user, f);
}

/***********************************************************************
 * Hooks of the life cycle (struct aworker_pool_user_hooks)
 ***********************************************************************/

static void hook_rx(struct aworker *w, const struct aworker_ipc_msg *m)
{
	switch (m->type) {
	case AWORKER_IPC_MT_USER_BIND_RESULT:
		handle_bind_result(w, m);
		return;
	case AWORKER_IPC_MT_SCU_UP:
		handle_scu_up(w, m);
		return;
	default:
		handle_tx_failed(w, m);
		return;
	}
}

static void hook_startup_report(struct aworker_pool *pool)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry) {
		if (au->bound)
			continue;
		LOGU(au, LOGL_ERROR, "still not bound %u s after start: no worker owns cs7 instance %u."
		     " Check that exactly one worker config file configures an 'asp' on it.\n",
		     pool->cfg.startup_timeout, au->cs7_id);
	}
}

static void hook_pool_stopped(struct aworker_pool *pool)
{
	struct aworker_user *au;

	while (!llist_empty(&pool->users)) {
		au = llist_first_entry(&pool->users, struct aworker_user, entry);
		llist_del(&au->entry);
		au->pool = NULL;
		au->worker = NULL;
		au->bind_sent = false;
		au->bound = false;
	}
}

static void hook_dump(struct vty *vty, struct aworker_pool *pool)
{
	struct aworker_user *au;

	llist_for_each_entry(au, &pool->users, entry) {
		vty_out(vty, " SCCP user '%s': cs7 instance %u, SSN %u, %s%s", au->name, au->cs7_id, au->ssn,
			au->bound ? "bound" : (au->bind_sent ? "bind pending" : "no worker"), VTY_NEWLINE);
	}
}

static const struct aworker_pool_user_hooks user_hooks = {
	.worker_ready = users_try_bind_all,
	.worker_lost = users_lose_worker,
	.rx = hook_rx,
	.startup_report = hook_startup_report,
	.pool_stopped = hook_pool_stopped,
	.dump = hook_dump,
};

/***********************************************************************
 * SCCP User backend: main process -> worker
 ***********************************************************************/

/* Common tail of all transmit operations: hand the record to the link. */
static int user_send(struct aworker_user *au, struct msgb *rec, uint8_t type, uint32_t conn_id)
{
	uint32_t seq = 0;
	int rc;

	rc = aworker_ipc_link_send(au->worker->link, rec, &seq);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot send %s (conn_id %u) to worker %u: %s\n",
		     aworker_ipc_msg_type_name(type), conn_id, au->worker->idx, strerror(-rc));
		return rc;
	}
	LOGU(au, LOGL_DEBUG, "sent %s (conn_id %u, seq %u) to worker %u\n", aworker_ipc_msg_type_name(type),
	     conn_id, seq, au->worker->idx);
	return 0;
}

/* NULL if this user cannot currently transmit. */
static struct aworker_user *tx_user(struct bsc_sccp_user *user)
{
	struct aworker_user *au = user->worker_priv;

	if (!au || !au->bound || !au->worker || !au->worker->link)
		return NULL;
	if (aworker_ipc_link_get_state(au->worker->link) != AWORKER_IPC_LINK_READY)
		return NULL;
	return au;
}

/* The SCCP operations are bounded by what the pinned libosmo-sigtran transmit path can encode; see
 * aworker_ipc_sccp_max_data(). Checking here keeps the error synchronous (the caller sees -EMSGSIZE and frees
 * nothing twice); the worker checks again before it calls the library. */
static int check_len(struct aworker_user *au, uint8_t type, unsigned int len)
{
	int rc = aworker_ipc_sccp_check_data(type, len);

	if (rc < 0)
		LOGU(au, LOGL_ERROR, "%s: %u octets of user data exceed the %d the A interface supports\n",
		     aworker_ipc_msg_type_name(type), len, aworker_ipc_sccp_max_data(type));
	return rc;
}

static int w_tx_unitdata_msg(struct bsc_sccp_user *user,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_UNITDATA, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	/* The encoder always takes ownership of msg, like osmo_sccp_tx_unitdata_msg(). */
	rc = aworker_ipc_enc_tx_unitdata(au->pool->ctx, &rec, au->user_ref, calling_addr, called_addr, msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_UNITDATA: %s\n", strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_UNITDATA, AWORKER_IPC_CONN_ID_NONE);
}

static int w_tx_conn_req_msg(struct bsc_sccp_user *user, uint32_t conn_id,
			     const struct osmo_sccp_addr *calling_addr,
			     const struct osmo_sccp_addr *called_addr,
			     struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_CONN_REQ, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	rc = aworker_ipc_enc_tx_conn_req(au->pool->ctx, &rec, au->user_ref, conn_id, calling_addr, called_addr,
					 msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_CONN_REQ (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_CONN_REQ, conn_id);
}

static int w_tx_conn_resp(struct bsc_sccp_user *user, uint32_t conn_id,
			  const struct osmo_sccp_addr *resp_addr,
			  const uint8_t *data, unsigned int len)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au)
		return -ENOTCONN;
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_CONN_RESP, len)) < 0)
		return rc;
	rc = aworker_ipc_enc_tx_conn_resp(au->pool->ctx, &rec, au->user_ref, conn_id, resp_addr, data, len);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_CONN_RESP (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_CONN_RESP, conn_id);
}

static int w_tx_data_msg(struct bsc_sccp_user *user, uint32_t conn_id, struct msgb *msg)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au) {
		msgb_free(msg);
		return -ENOTCONN;
	}
	if ((rc = check_len(au, AWORKER_IPC_MT_TX_DATA, msgb_length(msg))) < 0) {
		msgb_free(msg);
		return rc;
	}
	rc = aworker_ipc_enc_tx_data(au->pool->ctx, &rec, au->user_ref, conn_id, msg);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_DATA (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_DATA, conn_id);
}

static int w_tx_disconn(struct bsc_sccp_user *user, uint32_t conn_id,
			const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	struct aworker_user *au = tx_user(user);
	struct msgb *rec;
	int rc;

	if (!au)
		return -ENOTCONN;
	rc = aworker_ipc_enc_tx_disconn(au->pool->ctx, &rec, au->user_ref, conn_id, resp_addr, cause);
	if (rc < 0) {
		LOGU(au, LOGL_ERROR, "cannot encode TX_DISCONN (conn_id %u): %s\n", conn_id, strerror(-rc));
		return rc;
	}
	return user_send(au, rec, AWORKER_IPC_MT_TX_DISCONN, conn_id);
}

const struct bsc_sccp_backend_ops bsc_sccp_backend_worker_ops = {
	.name = "worker",
	.tx_unitdata_msg = w_tx_unitdata_msg,
	.tx_conn_req_msg = w_tx_conn_req_msg,
	.tx_conn_resp = w_tx_conn_resp,
	.tx_data_msg = w_tx_data_msg,
	.tx_disconn = w_tx_disconn,
};

/***********************************************************************
 * SCCP User binding
 ***********************************************************************/

struct bsc_sccp_user *aworker_sccp_user_bind(void *ctx, struct osmo_ss7_instance *ss7,
					     struct bsc_sccp_inst *bsc_sccp, const char *name,
					     bsc_sccp_user_prim_cb prim_cb, uint16_t ssn,
					     const struct aworker_sccp_user_ops *ops)
{
	struct bsc_sccp_user *user;
	struct aworker_user *au;
	uint32_t cs7_id;

	OSMO_ASSERT(ss7);
	OSMO_ASSERT(bsc_sccp);
	OSMO_ASSERT(prim_cb);
	OSMO_ASSERT(name && *name && strlen(name) <= AWORKER_IPC_MAX_USER_NAME);

	/* From the first user on, the life cycle reports to this side (struct aworker_pool_user_hooks). */
	aworker_pool_set_user_hooks(&user_hooks);

	if (!aworker_pool_get()) {
		LOGP(DAWORKER, LOGL_ERROR, "cannot bind SCCP user '%s': the A-worker pool is not running\n", name);
		return NULL;
	}
	cs7_id = osmo_ss7_instance_get_id(ss7);
	if (user_by_cs7_ssn(aworker_pool_get(), cs7_id, ssn)) {
		LOGP(DAWORKER, LOGL_ERROR, "cannot bind SCCP user '%s': cs7 instance %u SSN %u is already bound\n",
		     name, cs7_id, ssn);
		return NULL;
	}

	user = talloc_zero(ctx, struct bsc_sccp_user);
	if (!user)
		return NULL;
	au = talloc_zero(user, struct aworker_user);
	if (!au) {
		talloc_free(user);
		return NULL;
	}

	au->pool = aworker_pool_get();
	au->cs7_id = cs7_id;
	au->ssn = ssn;
	OSMO_STRLCPY_ARRAY(au->name, name);
	au->ops = ops;
	au->user = user;

	*user = (struct bsc_sccp_user){
		.ops = &bsc_sccp_backend_worker_ops,
		.prim_cb = prim_cb,
		.ss7 = ss7,
		.bsc_sccp = bsc_sccp,
		.worker_priv = au,
	};

	llist_add_tail(&au->entry, &aworker_pool_get()->users);
	LOGU(au, LOGL_NOTICE, "SCCP user registered for the A-worker pool\n");
	user_try_bind(au);
	return user;
}

struct bsc_sccp_user *aworker_sccp_user_find(uint32_t cs7_id, uint16_t ssn)
{
	struct aworker_user *au;

	if (!aworker_pool_get())
		return NULL;
	au = user_by_cs7_ssn(aworker_pool_get(), cs7_id, ssn);
	return au ? au->user : NULL;
}

void aworker_sccp_user_unbind(struct bsc_sccp_user *user)
{
	struct aworker_user *au;

	if (!user)
		return;
	au = user->worker_priv;
	if (au) {
		if (au->bind_sent && au->worker && au->worker->link) {
			struct msgb *rec;
			if (aworker_ipc_enc_user_unbind(au->pool->ctx, &rec, au->user_ref) == 0)
				aworker_ipc_link_send(au->worker->link, rec, NULL);
		}
		if (au->pool)
			llist_del(&au->entry);
	}
	talloc_free(user);
}

bool aworker_sccp_user_ready(const struct bsc_sccp_user *user)
{
	const struct aworker_user *au;

	if (!user || user->ops != &bsc_sccp_backend_worker_ops)
		return false;
	au = user->worker_priv;
	return au && au->bound;
}

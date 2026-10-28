/* A-worker: the worker process that owns the A-interface SIGTRAN stack (see aworker.h) */
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
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_helpers.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/vty/command.h>

#include <osmocom/bsc/aworker.h>
#include <osmocom/bsc/aworker_cfg_scan.h>
#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>

/* One cs7 instance this worker owns, with its SCCP instance. */
struct aworker_cs7 {
	struct llist_head entry;
	struct osmo_ss7_instance *ss7;
	uint32_t cs7_id;
	enum osmo_ss7_asp_protocol proto;
	unsigned int num_as;
	struct osmo_sccp_instance *sccp;
};

/* One SCCP User the main process asked this worker to bind. */
struct aworker_wuser {
	struct llist_head entry;
	uint32_t user_ref;		/*!< assigned by the main process */
	uint32_t cs7_id;
	uint16_t ssn;
	char name[AWORKER_IPC_MAX_USER_NAME + 1];
	struct aworker_cs7 *cs7;
	struct osmo_sccp_user *scu;
};

static struct {
	void *ctx;
	unsigned int idx;
	const char *cfg_file;
	struct aworker_ipc_link *link;
	struct llist_head cs7s;		/*!< struct aworker_cs7 */
	struct llist_head users;	/*!< struct aworker_wuser */
	int exit_code;
	bool link_down;
	/*! CS7_INFO, encoded once the cs7 instances are up and sent as soon as the handshake completes. */
	struct msgb *cs7_info;
} g_w;

#define LOGAW(lvl, fmt, args...) LOGP(DAWORKER, lvl, "aworker %u: " fmt, g_w.idx, ## args)
#define LOGWU(wu, lvl, fmt, args...) \
	LOGP(DAWORKER, lvl, "aworker %u user '%s' (cs7 %u ssn %u, ref %u): " fmt, g_w.idx, (wu)->name, \
	     (wu)->cs7_id, (wu)->ssn, (wu)->user_ref, ## args)

/***********************************************************************
 * Helpers
 ***********************************************************************/

static struct aworker_cs7 *cs7_by_id(uint32_t cs7_id)
{
	struct aworker_cs7 *c;

	llist_for_each_entry(c, &g_w.cs7s, entry) {
		if (c->cs7_id == cs7_id)
			return c;
	}
	return NULL;
}

static struct aworker_wuser *user_by_ref(uint32_t user_ref)
{
	struct aworker_wuser *wu;

	llist_for_each_entry(wu, &g_w.users, entry) {
		if (wu->user_ref == user_ref)
			return wu;
	}
	return NULL;
}

static struct aworker_wuser *user_by_cs7_ssn(uint32_t cs7_id, uint16_t ssn)
{
	struct aworker_wuser *wu;

	llist_for_each_entry(wu, &g_w.users, entry) {
		if (wu->cs7_id == cs7_id && wu->ssn == ssn)
			return wu;
	}
	return NULL;
}

/* Send one encoded record; a send failure takes the link (and with it the worker) down. */
static void link_send(struct msgb *rec, const char *what)
{
	uint32_t seq = 0;
	int rc;

	rc = aworker_ipc_link_send(g_w.link, rec, &seq);
	if (rc < 0)
		LOGAW(LOGL_ERROR, "cannot send %s to the main process: %s\n", what, strerror(-rc));
}

static void tx_failed(const struct aworker_wuser *wu, uint32_t ref_seq, uint8_t ref_type, uint8_t disposition,
		      uint16_t code, uint32_t conn_id, int32_t lib_rc, uint8_t post_conn)
{
	struct aworker_ipc_tx_failed f = {
		.ref_seq = ref_seq,
		.ref_type = ref_type,
		.disposition = disposition,
		.code = code,
		.conn_id = conn_id,
		.lib_rc = lib_rc,
		.post_conn = post_conn,
	};
	struct msgb *rec;
	int rc;

	rc = aworker_ipc_enc_tx_failed(g_w.ctx, &rec, wu->user_ref, &f);
	if (rc < 0) {
		LOGWU(wu, LOGL_ERROR, "cannot encode TX_FAILED for %s: %s\n",
		      aworker_ipc_msg_type_name(ref_type), strerror(-rc));
		return;
	}
	link_send(rec, "TX_FAILED");
}

/* Report that the libosmo-sigtran helper was not called at all. */
static void tx_not_executed(const struct aworker_wuser *wu, const struct aworker_ipc_msg *m, uint16_t code,
			    uint32_t conn_id)
{
	LOGWU(wu, LOGL_ERROR, "%s (seq %u, conn_id %u) not executed: code %u\n",
	      aworker_ipc_msg_type_name(m->type), m->seq, conn_id, code);
	tx_failed(wu, m->seq, m->type, AWORKER_IPC_TXF_NOT_EXECUTED, code, conn_id, 0,
		  AWORKER_IPC_POST_CONN_NA);
}

/* Report the result of a libosmo-sigtran helper that was called and failed. */
static void tx_executed(const struct aworker_wuser *wu, const struct aworker_ipc_msg *m, int lib_rc,
			uint32_t conn_id)
{
	uint8_t post_conn = AWORKER_IPC_POST_CONN_NA;

	if (lib_rc >= 0)
		return;
	if (m->type != AWORKER_IPC_MT_TX_UNITDATA)
		post_conn = osmo_sccp_conn_id_exists(wu->cs7->sccp, conn_id) ? AWORKER_IPC_POST_CONN_PRESENT
									    : AWORKER_IPC_POST_CONN_ABSENT;
	LOGWU(wu, LOGL_ERROR, "%s (seq %u, conn_id %u) failed in libosmo-sigtran: rc %d."
	      " Whether anything reached the network is unknown; it is NOT retried here.\n",
	      aworker_ipc_msg_type_name(m->type), m->seq, conn_id, lib_rc);
	tx_failed(wu, m->seq, m->type, AWORKER_IPC_TXF_EXECUTED_ERROR, AWORKER_IPC_TXF_LIB_ERROR, conn_id,
		  lib_rc, post_conn);
}

/* A msgb holding exactly the DATA of a received record, for the osmo_sccp_tx_*_msg() helpers. On allocation
 * failure the link is closed (and the worker exits), as everywhere else in this IPC: there is no protocol code
 * for "out of memory", and silently reporting a different reason would misinform the main process. */
static struct msgb *data_to_msgb(const struct aworker_ipc_data *d)
{
	struct msgb *msg = msgb_alloc_c(g_w.ctx, OSMO_MAX(d->len, 1), "aworker-ipc tx data");

	if (!msg) {
		LOGAW(LOGL_FATAL, "cannot allocate a %u octet msgb for a transmit request; closing link\n",
		      d->len);
		aworker_ipc_link_close(g_w.link);
		return NULL;
	}
	if (d->len)
		memcpy(msgb_put(msg, d->len), d->buf, d->len);
	return msg;
}

/***********************************************************************
 * SCCP -> main process
 ***********************************************************************/

/* osmo_prim_cb of every SCCP user this worker binds: forward the primitive to the main process. */
static int worker_sap_up(struct osmo_prim_hdr *oph, void *_scu)
{
	struct osmo_sccp_user *scu = _scu;
	struct aworker_wuser *wu = osmo_sccp_user_get_priv(scu);
	struct osmo_scu_prim *prim = (struct osmo_scu_prim *)oph;
	const uint8_t *data = NULL;
	unsigned int len = 0;
	struct msgb *rec;
	int rc;

	OSMO_ASSERT(wu);

	/* N-STATE.ind and N-PCSTATE.ind carry no user data in this protocol (and osmo-bsc reads none). */
	if (oph->primitive != OSMO_SCU_PRIM_N_STATE && oph->primitive != OSMO_SCU_PRIM_N_PCSTATE
	    && oph->msg && msgb_l2(oph->msg) && msgb_l2len(oph->msg) > 0) {
		data = msgb_l2(oph->msg);
		len = msgb_l2len(oph->msg);
	}

	rc = aworker_ipc_enc_scu_up(g_w.ctx, &rec, wu->user_ref, prim, data, len);
	if (rc < 0) {
		/* -ENOTSUP: a primitive the protocol does not carry (and that osmo-bsc does not act on either);
		 * -EMSGSIZE: more user data than the IPC transport carries. Both are dropped, loudly. */
		LOGWU(wu, rc == -ENOTSUP ? LOGL_INFO : LOGL_ERROR,
		      "dropping %s.%s (%u octets of data): %s\n", osmo_scu_prim_type_name(oph->primitive),
		      get_value_string(osmo_prim_op_names, oph->operation), len, strerror(-rc));
		msgb_free(oph->msg);
		return 0;
	}
	link_send(rec, "SCU_UP");
	msgb_free(oph->msg);
	return 0;
}

/***********************************************************************
 * main process -> SCCP
 ***********************************************************************/

static void handle_user_bind(const struct aworker_ipc_msg *m)
{
	const struct aworker_ipc_user_bind *b = &m->u.user_bind;
	struct aworker_wuser *wu;
	struct aworker_cs7 *cs7;
	struct msgb *rec;
	uint16_t result;
	int rc;

	if (user_by_ref(m->user_ref)) {
		LOGAW(LOGL_ERROR, "USER_BIND with user_ref %u already in use; closing link\n", m->user_ref);
		aworker_ipc_link_close(g_w.link);
		return;
	}

	cs7 = cs7_by_id(b->cs7_id);
	if (!cs7) {
		LOGAW(LOGL_ERROR, "USER_BIND '%s' for cs7 instance %u, which this worker does not own\n",
		      b->name, b->cs7_id);
		result = AWORKER_IPC_BIND_NO_SUCH_CS7;
		goto reply;
	}
	if (user_by_cs7_ssn(b->cs7_id, b->ssn)) {
		LOGAW(LOGL_ERROR, "USER_BIND '%s': cs7 instance %u SSN %u is already bound\n", b->name,
		      b->cs7_id, b->ssn);
		result = AWORKER_IPC_BIND_ALREADY_BOUND;
		goto reply;
	}

	wu = talloc_zero(g_w.ctx, struct aworker_wuser);
	if (!wu) {
		result = AWORKER_IPC_BIND_FAILED;
		goto reply;
	}
	wu->user_ref = m->user_ref;
	wu->cs7_id = b->cs7_id;
	wu->ssn = b->ssn;
	OSMO_STRLCPY_ARRAY(wu->name, b->name);
	wu->cs7 = cs7;

	wu->scu = osmo_sccp_user_bind(cs7->sccp, wu->name, worker_sap_up, wu->ssn);
	if (!wu->scu) {
		LOGAW(LOGL_ERROR, "USER_BIND '%s': osmo_sccp_user_bind() failed for cs7 instance %u SSN %u\n",
		      b->name, b->cs7_id, b->ssn);
		talloc_free(wu);
		result = AWORKER_IPC_BIND_FAILED;
		goto reply;
	}
	osmo_sccp_user_set_priv(wu->scu, wu);
	llist_add_tail(&wu->entry, &g_w.users);
	LOGWU(wu, LOGL_NOTICE, "SCCP user bound\n");
	result = AWORKER_IPC_BIND_OK;

reply:
	rc = aworker_ipc_enc_user_bind_result(g_w.ctx, &rec, m->user_ref, result);
	if (rc < 0) {
		LOGAW(LOGL_ERROR, "cannot encode USER_BIND_RESULT: %s\n", strerror(-rc));
		return;
	}
	link_send(rec, "USER_BIND_RESULT");
}

static void user_free(struct aworker_wuser *wu)
{
	llist_del(&wu->entry);
	if (wu->scu)
		osmo_sccp_user_unbind(wu->scu);
	talloc_free(wu);
}

static void handle_user_unbind(const struct aworker_ipc_msg *m)
{
	struct aworker_wuser *wu = user_by_ref(m->user_ref);

	if (!wu) {
		LOGAW(LOGL_NOTICE, "USER_UNBIND for unknown user_ref %u, ignoring\n", m->user_ref);
		return;
	}
	LOGWU(wu, LOGL_NOTICE, "SCCP user unbound\n");
	user_free(wu);
}

static void handle_tx(const struct aworker_ipc_msg *m)
{
	struct aworker_wuser *wu = user_by_ref(m->user_ref);
	uint32_t conn_id = AWORKER_IPC_CONN_ID_NONE;
	struct msgb *data;
	int rc;

	switch (m->type) {
	case AWORKER_IPC_MT_TX_CONN_REQ:
		conn_id = m->u.tx_conn_req.conn_id;
		break;
	case AWORKER_IPC_MT_TX_CONN_RESP:
		conn_id = m->u.tx_conn_resp.conn_id;
		break;
	case AWORKER_IPC_MT_TX_DATA:
		conn_id = m->u.tx_data.conn_id;
		break;
	case AWORKER_IPC_MT_TX_DISCONN:
		conn_id = m->u.tx_disconn.conn_id;
		break;
	default:
		break;
	}

	if (!wu) {
		/* No user: there is nobody to attribute a TX_FAILED to either, so just report it on the link. */
		struct aworker_ipc_tx_failed f = {
			.ref_seq = m->seq,
			.ref_type = m->type,
			.disposition = AWORKER_IPC_TXF_NOT_EXECUTED,
			.code = AWORKER_IPC_TXF_NO_SUCH_USER,
			.conn_id = conn_id,
		};
		struct msgb *rec;

		LOGAW(LOGL_ERROR, "%s (seq %u) for unknown user_ref %u\n", aworker_ipc_msg_type_name(m->type),
		      m->seq, m->user_ref);
		if (aworker_ipc_enc_tx_failed(g_w.ctx, &rec, m->user_ref, &f) == 0)
			link_send(rec, "TX_FAILED");
		return;
	}

	/* The transport accepts more user data than the pinned libosmo-sigtran transmit path can encode. The main
	 * process checks this too; checking again here is what keeps a malformed or oversized request from
	 * reaching an encoder that would panic, drop the data or emit a wrong length. */
	if (m->type != AWORKER_IPC_MT_TX_DISCONN
	    && aworker_ipc_sccp_check_data(m->type, m->data.len) < 0) {
		tx_not_executed(wu, m, AWORKER_IPC_TXF_DATA_TOO_LONG, conn_id);
		return;
	}

	switch (m->type) {
	case AWORKER_IPC_MT_TX_UNITDATA:
		if (!(data = data_to_msgb(&m->data)))
			return;
		rc = osmo_sccp_tx_unitdata_msg(wu->scu, &m->u.tx_unitdata.calling, &m->u.tx_unitdata.called,
					       data);
		tx_executed(wu, m, rc, conn_id);
		return;

	case AWORKER_IPC_MT_TX_CONN_REQ:
		if (osmo_sccp_conn_id_exists(wu->cs7->sccp, conn_id)) {
			tx_not_executed(wu, m, AWORKER_IPC_TXF_CONN_ID_IN_USE, conn_id);
			return;
		}
		if (!(data = data_to_msgb(&m->data)))
			return;
		rc = osmo_sccp_tx_conn_req_msg(wu->scu, conn_id,
					       m->u.tx_conn_req.calling_present ? &m->u.tx_conn_req.calling
										: NULL,
					       &m->u.tx_conn_req.called, data);
		tx_executed(wu, m, rc, conn_id);
		return;

	case AWORKER_IPC_MT_TX_CONN_RESP:
		if (!osmo_sccp_conn_id_exists(wu->cs7->sccp, conn_id)) {
			tx_not_executed(wu, m, AWORKER_IPC_TXF_CONN_UNKNOWN, conn_id);
			return;
		}
		rc = osmo_sccp_tx_conn_resp(wu->scu, conn_id, &m->u.tx_conn_resp.resp, m->data.buf, m->data.len);
		tx_executed(wu, m, rc, conn_id);
		return;

	case AWORKER_IPC_MT_TX_DATA:
		if (!osmo_sccp_conn_id_exists(wu->cs7->sccp, conn_id)) {
			tx_not_executed(wu, m, AWORKER_IPC_TXF_CONN_UNKNOWN, conn_id);
			return;
		}
		if (!(data = data_to_msgb(&m->data)))
			return;
		rc = osmo_sccp_tx_data_msg(wu->scu, conn_id, data);
		tx_executed(wu, m, rc, conn_id);
		return;

	case AWORKER_IPC_MT_TX_DISCONN:
		if (!osmo_sccp_conn_id_exists(wu->cs7->sccp, conn_id)) {
			tx_not_executed(wu, m, AWORKER_IPC_TXF_CONN_UNKNOWN, conn_id);
			return;
		}
		rc = osmo_sccp_tx_disconn(wu->scu, conn_id,
					  m->u.tx_disconn.resp_present ? &m->u.tx_disconn.resp : NULL,
					  m->u.tx_disconn.cause);
		tx_executed(wu, m, rc, conn_id);
		return;

	default:
		OSMO_ASSERT(0);
	}
}

/***********************************************************************
 * IPC link call-backs
 ***********************************************************************/

static void send_cs7_info(void);

static void w_ready_cb(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	LOGAW(LOGL_INFO, "IPC handshake with the main process complete (pid %u, ident '%s')\n", peer->pid,
	      peer->ident);
	send_cs7_info();
}

static void w_rx_cb(struct aworker_ipc_link *link, const struct aworker_ipc_msg *m)
{
	switch (m->type) {
	case AWORKER_IPC_MT_USER_BIND:
		handle_user_bind(m);
		return;
	case AWORKER_IPC_MT_USER_UNBIND:
		handle_user_unbind(m);
		return;
	case AWORKER_IPC_MT_TX_UNITDATA:
	case AWORKER_IPC_MT_TX_CONN_REQ:
	case AWORKER_IPC_MT_TX_CONN_RESP:
	case AWORKER_IPC_MT_TX_DATA:
	case AWORKER_IPC_MT_TX_DISCONN:
		handle_tx(m);
		return;
	case AWORKER_IPC_MT_GOODBYE:
		LOGAW(LOGL_NOTICE, "the main process says GOODBYE (reason %u); shutting down\n",
		      m->u.goodbye_reason);
		g_w.exit_code = AWORKER_EXIT_OK;
		aworker_ipc_link_close(link);
		return;
	default:
		/* aworker_ipc_dec() already rejected everything a worker may not receive. */
		LOGAW(LOGL_ERROR, "unexpected %s from the main process; closing link\n",
		      aworker_ipc_msg_type_name(m->type));
		aworker_ipc_link_close(link);
		return;
	}
}

static void w_closed_cb(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	LOGAW(info->reason == AWORKER_IPC_CLOSE_LOCAL ? LOGL_NOTICE : LOGL_ERROR,
	      "IPC link closed: %s (%d), last record written %u, last queued %u\n",
	      aworker_ipc_close_reason_name(info->reason), info->err, info->last_written_seq,
	      info->last_queued_seq);
	if (info->reason != AWORKER_IPC_CLOSE_LOCAL)
		g_w.exit_code = AWORKER_EXIT_LINK;
	g_w.link_down = true;
	osmo_select_shutdown_request();
}

static const struct aworker_ipc_link_ops w_link_ops = {
	.ready_cb = w_ready_cb,
	.rx_cb = w_rx_cb,
	.closed_cb = w_closed_cb,
};

/***********************************************************************
 * Start-up
 ***********************************************************************/

static void w_signal_handler(int sig)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		/* Asynchronous-signal-safe enough for a shutdown request flag. */
		osmo_select_shutdown_request();
		break;
	default:
		break;
	}
}

/* The cs7 instances this worker owns are exactly those for which its own config file defines an asp, as or
 * listen node: reading those is what created the ASPs and opened the SCTP associations in this process. The
 * public libosmo-sigtran API cannot enumerate ASPs, and the usual osmo-bsc configuration defines only an 'asp'
 * and lets osmo_sccp_simple_client_on_ss7_id() create the AS, so the list and the protocol are taken from the
 * file (see aworker_cfg_scan.h) and then checked against what the VTY parser actually created. */
static int cs7_collect(void)
{
	struct aworker_cfg_scan scan;
	unsigned int i, count = 0;
	int rc;

	rc = aworker_cfg_scan(g_w.cfg_file, &scan);
	if (rc < 0) {
		LOGAW(LOGL_FATAL, "cannot read the worker config file '%s': %s\n", g_w.cfg_file, strerror(-rc));
		return rc;
	}
	if (scan.num_workers) {
		LOGAW(LOGL_FATAL, "the worker config file '%s' has an 'a-worker-pool' section. A worker must not"
		      " start workers of its own; that section belongs in the main configuration file.\n",
		      g_w.cfg_file);
		return -EINVAL;
	}

	for (i = 0; i < scan.num_cs7; i++) {
		const struct aworker_cfg_scan_cs7 *sc = &scan.cs7[i];
		struct osmo_ss7_instance *inst;
		struct aworker_cs7 *c;

		if (!sc->num_asp && !sc->num_as && !sc->num_listen)
			continue;	/* configuration only (point code, address book): nothing to own */

		if (sc->proto_conflict) {
			LOGAW(LOGL_FATAL, "cs7 instance %u (line %u) names more than one ASP protocol. One cs7"
			      " instance of the A interface must use exactly one protocol.\n", sc->id, sc->lineno);
			return -EINVAL;
		}
		if (sc->asp_proto == OSMO_SS7_ASP_PROT_NONE) {
			LOGAW(LOGL_FATAL, "cannot tell which protocol cs7 instance %u uses: its '%s' node on"
			      " line %u names none.\n", sc->id, sc->node, sc->node_lineno);
			return -EINVAL;
		}

		/* SUA transmit is not usable in the pinned libosmo-sigtran (2.3.0): xua_as_encode_msg() in
		 * src/xua_as_fsm.c only encodes M3UA and IPA and calls OSMO_ASSERT(0) for an SUA Application
		 * Server, so the first message sent towards an SUA peer aborts the process. Refuse the
		 * configuration instead of claiming support for it. */
		if (sc->asp_proto == OSMO_SS7_ASP_PROT_SUA) {
			LOGAW(LOGL_FATAL, "cs7 instance %u is configured for SUA, which the installed"
			      " libosmo-sigtran cannot transmit (xua_as_encode_msg() asserts for an SUA"
			      " Application Server). Use M3UA, or IPA for SCCPlite.\n", sc->id);
			return -ENOTSUP;
		}

		inst = osmo_ss7_instance_find(sc->id);
		if (!inst) {
			LOGAW(LOGL_FATAL, "cs7 instance %u was not created while reading '%s'\n", sc->id,
			      g_w.cfg_file);
			return -EINVAL;
		}
		if (count >= AWORKER_IPC_MAX_CS7) {
			LOGAW(LOGL_FATAL, "more than %u cs7 instances in '%s'; split them over more workers\n",
			      AWORKER_IPC_MAX_CS7, g_w.cfg_file);
			return -E2BIG;
		}

		c = talloc_zero(g_w.ctx, struct aworker_cs7);
		if (!c)
			return -ENOMEM;
		c->ss7 = inst;
		c->cs7_id = sc->id;
		c->proto = sc->asp_proto;
		c->num_as = sc->num_as;
		llist_add_tail(&c->entry, &g_w.cs7s);
		count++;
	}

	if (!count) {
		LOGAW(LOGL_FATAL, "config file '%s' defines no cs7 instance with an asp, as or listen node."
		      " A worker config file must contain the cs7 instance(s) this worker owns, with their"
		      " asp / as nodes.\n", g_w.cfg_file);
		return -EINVAL;
	}
	return 0;
}

/* Default point-code of the BSC, identical to the in-process mode (osmo_bsc_sigtran.c). */
#define BSC_DEFAULT_PC "0.23.3"

static int cs7_setup_sccp(void)
{
	struct aworker_cs7 *c;

	llist_for_each_entry(c, &g_w.cs7s, entry) {
		enum osmo_ss7_asp_protocol p;
		char inst_name[32];

		snprintf(inst_name, sizeof(inst_name), "A-%u-%s", c->cs7_id,
			 osmo_ss7_asp_protocol_name(c->proto));
		LOGAW(LOGL_NOTICE, "initializing SCCP for A/%s on cs7 instance %u\n",
		      osmo_ss7_asp_protocol_name(c->proto), c->cs7_id);

		/* The same call the in-process mode makes: it reuses the ASP and AS the config file created
		 * (which libosmo-sigtran already started while reading it) and attaches an SCCP instance. */
		c->sccp = osmo_sccp_simple_client_on_ss7_id(g_w.ctx, c->cs7_id, inst_name,
							    osmo_ss7_pointcode_parse(NULL, BSC_DEFAULT_PC),
							    c->proto, 0, NULL, 0, NULL);
		if (!c->sccp) {
			LOGAW(LOGL_FATAL, "cannot set up SCCP on cs7 instance %u. If the asp node of that"
			      " instance comes from the config file, it must set both 'role' and"
			      " 'transport-role'.\n", c->cs7_id);
			return -EINVAL;
		}
		/* Count what libosmo-sigtran really has now: the config file may define the Application
		 * Server(s), or osmo_sccp_simple_client_on_ss7_id() may just have created one. */
		c->num_as = 0;
		for (p = OSMO_SS7_ASP_PROT_SUA; p < _NUM_OSMO_SS7_ASP_PROT; p++) {
			if (osmo_ss7_as_find_by_proto(c->ss7, p))
				c->num_as++;
		}
	}
	return 0;
}

/* Encode CS7_INFO. It is sent from w_ready_cb(), i.e. as soon as the IPC handshake completes: the link rejects
 * anything sent before that, and this function can run before the main process' HELLO has arrived. */
static int encode_cs7_info(void)
{
	struct aworker_ipc_cs7_info info = {};
	struct aworker_cs7 *c;
	int rc;

	llist_for_each_entry(c, &g_w.cs7s, entry) {
		struct aworker_ipc_cs7_entry *e = &info.e[info.count++];
		e->cs7_id = c->cs7_id;
		e->primary_pc = osmo_ss7_instance_get_primary_pc(c->ss7);
		e->asp_proto = c->proto;
		e->num_as = c->num_as > 255 ? 255 : (uint8_t)c->num_as;
	}

	rc = aworker_ipc_enc_cs7_info(g_w.ctx, &g_w.cs7_info, &info);
	if (rc < 0)
		LOGAW(LOGL_FATAL, "cannot encode CS7_INFO: %s\n", strerror(-rc));
	return rc;
}

static void send_cs7_info(void)
{
	struct msgb *rec = g_w.cs7_info;

	if (!rec)
		return;
	g_w.cs7_info = NULL;
	link_send(rec, "CS7_INFO");
}

static void worker_cleanup(void)
{
	struct aworker_wuser *wu, *wu2;

	llist_for_each_entry_safe(wu, wu2, &g_w.users, entry)
		user_free(wu);
	msgb_free(g_w.cs7_info);
	g_w.cs7_info = NULL;
	if (g_w.link) {
		/* Only once the handshake is complete: the link rejects anything sent before that, and there is
		 * nothing to say goodbye to if the main process never answered. */
		if (!g_w.link_down && aworker_ipc_link_get_state(g_w.link) == AWORKER_IPC_LINK_READY) {
			struct msgb *rec;
			if (aworker_ipc_enc_goodbye(g_w.ctx, &rec, AWORKER_IPC_GOODBYE_SHUTDOWN) == 0)
				link_send(rec, "GOODBYE");
		}
		aworker_ipc_link_free(g_w.link);
		g_w.link = NULL;
	}
}

void aworker_run(int fd, unsigned int worker_idx, const char *cfg_file)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = AWORKER_IPC_ROLE_WORKER,
		.worker_idx = (uint16_t)worker_idx,
		.ident = "osmo-bsc",
		.log_cat = DAWORKER,
	};
	int rc;

	g_w.ctx = talloc_named_const(tall_bsc_ctx, 0, "aworker");
	OSMO_ASSERT(g_w.ctx);
	g_w.idx = worker_idx;
	g_w.cfg_file = cfg_file;
	g_w.exit_code = AWORKER_EXIT_OK;
	INIT_LLIST_HEAD(&g_w.cs7s);
	INIT_LLIST_HEAD(&g_w.users);
	cfg.pid = (uint32_t)getpid();

	/* aworker_ipc_link_alloc() refuses to run unless SIGPIPE is ignored. */
	osmo_init_ignore_signals();
	signal(SIGINT, w_signal_handler);
	signal(SIGTERM, w_signal_handler);

	LOGAW(LOGL_NOTICE, "A-worker starting: pid %ld, IPC fd %d, config file '%s'\n", (long)getpid(), fd,
	      cfg_file);

	rc = aworker_ipc_link_alloc(&g_w.link, g_w.ctx, fd, &cfg, &w_link_ops, NULL);
	if (rc < 0) {
		LOGAW(LOGL_FATAL, "cannot set up the IPC link on fd %d: %s\n", fd, strerror(-rc));
		exit(AWORKER_EXIT_LINK);
	}

	/* Reading this file is what creates the ASPs and opens the SCTP associations -- in this process only. */
	rc = vty_read_config_file(cfg_file, NULL);
	if (rc < 0) {
		LOGAW(LOGL_FATAL, "cannot parse the worker config file '%s': %s\n", cfg_file, strerror(-rc));
		worker_cleanup();
		exit(AWORKER_EXIT_CONFIG);
	}

	if (cs7_collect() < 0 || cs7_setup_sccp() < 0) {
		worker_cleanup();
		exit(AWORKER_EXIT_CONFIG);
	}
	if (encode_cs7_info() < 0) {
		worker_cleanup();
		exit(AWORKER_EXIT_CONFIG);
	}
	/* If the handshake already completed while the config file was read, send it now. */
	if (aworker_ipc_link_get_state(g_w.link) == AWORKER_IPC_LINK_READY)
		send_cs7_info();

	while (!osmo_select_shutdown_done())
		osmo_select_main_ctx(0);

	LOGAW(LOGL_NOTICE, "A-worker shutting down (exit code %d)\n", g_w.exit_code);
	worker_cleanup();
	exit(g_w.exit_code);
}

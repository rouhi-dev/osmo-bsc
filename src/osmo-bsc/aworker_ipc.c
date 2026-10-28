/* A-worker IPC: wire codec (see the protocol description in aworker_ipc.h) */
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
#include <string.h>

#include <osmocom/core/bit16gen.h>
#include <osmocom/core/bit32gen.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/prim.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/sigtran_sap.h>
#include <osmocom/sigtran/protocol/m3ua.h>
#include <osmocom/sccp/sccp_types.h>

#include <osmocom/bsc/aworker_ipc.h>

/* Per-operation SCCP user data limits, see aworker_ipc_sccp_max_data(). Derived from the pinned libosmo-sigtran
 * (2.3.0) and libosmocore (1.14.2) transmit paths; the private buffer sizes are mirrored here.
 *
 * Every msgb_put() below panics on overflow (MSGB_DEBUG is always defined in <osmocom/core/msgb.h>).
 *
 * N-UNITDATA.req: osmo_sccp_tx_unitdata() -> SCLC builds a SUA CLDT (sccp_sclc.c xua_gen_msg_cl()) -> SCRC
 * (sccp_scrc.c gen_mtp_transfer_req_xua()):
 *  - M3UA and IPA: osmo_sua_to_sccp() encodes an SCCP UDT, or an LUDT if the data exceeds 255 octets
 *    (sccp2sua.c sua_to_sccp_udt()/sua_to_sccp_ludt()). The LUDT is the fixed part, two addresses with a 1 octet
 *    length each, a 2 octet data length and the data; the SCLC adds no LUDT optional parameter. An SCCP address is at
 *    most 23 octets (osmo_sccp_addr_encode(): indicator, 14 bit PC, SSN, TT + NP/ES + NAI, 16 octets for the at most
 *    31 BCD digits; IP addresses are not encoded).
 *    - M3UA (m3ua_xfer_from_data(), m3ua_tx_xua_as(), xua_to_msg()): one msgb_alloc_headroom(2048, 512), i.e. 1536
 *      octets for the 8 octet common header, an 8 octet Routing Context (if configured) and the Protocol Data part
 *      (4 octet tag/length, 12 octet MTP header, the SCCP message, padding to 4 octets).
 *    - IPA (ipa_to_msg()): the SCCP message is copied into ipa_msg_alloc(16): 1200 octets (IPA_ALLOC_SIZE,
 *      libosmocore src/gsm/ipa.c).
 *  - SUA: sua_tx_xua_as() -> xua_as_transmit_msg() -> xua_as_encode_msg() (xua_as_fsm.c), which only encodes M3UA and
 *    IPA and hits OSMO_ASSERT(0) for an SUA AS: no size is safe for SUA in this libosmo-sigtran version.
 *  The helper's own buffer (2048 octets including the 264 octet struct osmo_scu_prim on LP64, SCU_MSG_SIZE in
 *  sccp_helpers.c) is larger than both limits.
 *
 * N-CONNECT.req/.resp and N-DATA.req: user data of a CR or CC is dropped if it exceeds SCCP_MAX_DATA (256) and is
 * otherwise sent later in a DT1 if it exceeds the instance's max-optional-data (sccp_scoc.c xua_opt_data_cache_keep(),
 * sccp_connection.c); a DT1 stores the data length in one octet (sccp2sua.c sccp_add_variable_part()), so 256 octets
 * are encoded with length 0. 255 is the largest length that is neither dropped nor malformed. */
#define SIGTRAN_XUA_TX_BUF		(2048 - 512)
#define SIGTRAN_IPA_TX_BUF		1200
#define SCCP_ADDR_MAX_LEN		(sizeof(struct sccp_called_party_address) + 2 + 1 + 3 + AWORKER_IPC_DIGITS_LEN / 2)
#define SCCP_LUDT_OVERHEAD		(sizeof(struct sccp_data_long_unitdata) + 2 * (1 + SCCP_ADDR_MAX_LEN) + 2)
#define XUA_COMMON_HDR_LEN		8
#define XUA_PART_HDR_LEN		4
#define XUA_U32_PART_LEN		8
#define UNITDATA_MAX_M3UA		(((SIGTRAN_XUA_TX_BUF - XUA_COMMON_HDR_LEN - XUA_U32_PART_LEN - XUA_PART_HDR_LEN) & ~3) \
					 - sizeof(struct m3ua_data_hdr) - SCCP_LUDT_OVERHEAD)
#define UNITDATA_MAX_IPA		(SIGTRAN_IPA_TX_BUF - SCCP_LUDT_OVERHEAD)
#define UNITDATA_MAX			OSMO_MIN(UNITDATA_MAX_M3UA, UNITDATA_MAX_IPA)
#define CO_DATA_MAX			(SCCP_MAX_DATA - 1)

osmo_static_assert(UNITDATA_MAX > 255, unitdata_limit_needs_ludt_path);
osmo_static_assert(UNITDATA_MAX + sizeof(struct osmo_scu_prim) <= 2048, unitdata_limit_within_helper_buffer);

/* Allocation sizes of the msgbs in which libosmo-sigtran passes primitives up to the SCCP user, mirrored so that a
 * primitive rebuilt from IPC has the same tailroom/headroom as an in-process one:
 * connection-oriented: msgb_alloc(1024) in src/sccp_connection.c; connectionless and local broadcast:
 * msgb_alloc_headroom(2048 + 512, 512) (sccp_msgb_alloc() in src/sua.c). */
#define SIGTRAN_UP_CO_SIZE		1024
#define SIGTRAN_UP_CL_SIZE		2048
#define SIGTRAN_UP_CL_HEADROOM		512

#define HELLO_FIXED_LEN			13
#define TX_FAILED_LEN			20
#define FLAG_ADDR_PRESENT		0x01

static const struct value_string err_names[] = {
	{ AWORKER_IPC_ERR_NONE, "none" },
	{ AWORKER_IPC_ERR_FRAME_TOO_SHORT, "frame-too-short" },
	{ AWORKER_IPC_ERR_FRAME_TOO_LONG, "frame-too-long" },
	{ AWORKER_IPC_ERR_BAD_MAGIC, "bad-magic" },
	{ AWORKER_IPC_ERR_BAD_VERSION, "bad-version" },
	{ AWORKER_IPC_ERR_BAD_FLAGS, "bad-flags" },
	{ AWORKER_IPC_ERR_BAD_BODY_LEN, "bad-body-len" },
	{ AWORKER_IPC_ERR_UNKNOWN_TYPE, "unknown-type" },
	{ AWORKER_IPC_ERR_WRONG_DIRECTION, "wrong-direction" },
	{ AWORKER_IPC_ERR_BAD_USER_REF, "bad-user-ref" },
	{ AWORKER_IPC_ERR_TRUNCATED, "truncated" },
	{ AWORKER_IPC_ERR_TRAILING_DATA, "trailing-data" },
	{ AWORKER_IPC_ERR_BAD_RESERVED, "bad-reserved" },
	{ AWORKER_IPC_ERR_BAD_ADDR, "bad-addr" },
	{ AWORKER_IPC_ERR_BAD_CONN_ID, "bad-conn-id" },
	{ AWORKER_IPC_ERR_DATA_TOO_LONG, "data-too-long" },
	{ AWORKER_IPC_ERR_UNEXPECTED_DATA, "unexpected-data" },
	{ AWORKER_IPC_ERR_BAD_PRIM, "bad-prim" },
	{ AWORKER_IPC_ERR_BAD_VALUE, "bad-value" },
	{ AWORKER_IPC_ERR_BAD_STRING, "bad-string" },
	{ 0, NULL }
};

const char *aworker_ipc_err_name(int err)
{
	if (err < 0)
		err = -err;
	return get_value_string(err_names, err);
}

static const struct value_string msg_type_names[] = {
	{ AWORKER_IPC_MT_HELLO, "HELLO" },
	{ AWORKER_IPC_MT_ECHO_REQ, "ECHO_REQ" },
	{ AWORKER_IPC_MT_ECHO_RSP, "ECHO_RSP" },
	{ AWORKER_IPC_MT_GOODBYE, "GOODBYE" },
	{ AWORKER_IPC_MT_CS7_INFO, "CS7_INFO" },
	{ AWORKER_IPC_MT_USER_BIND, "USER_BIND" },
	{ AWORKER_IPC_MT_USER_UNBIND, "USER_UNBIND" },
	{ AWORKER_IPC_MT_USER_BIND_RESULT, "USER_BIND_RESULT" },
	{ AWORKER_IPC_MT_TX_UNITDATA, "TX_UNITDATA" },
	{ AWORKER_IPC_MT_TX_CONN_REQ, "TX_CONN_REQ" },
	{ AWORKER_IPC_MT_TX_CONN_RESP, "TX_CONN_RESP" },
	{ AWORKER_IPC_MT_TX_DATA, "TX_DATA" },
	{ AWORKER_IPC_MT_TX_DISCONN, "TX_DISCONN" },
	{ AWORKER_IPC_MT_SCU_UP, "SCU_UP" },
	{ AWORKER_IPC_MT_TX_FAILED, "TX_FAILED" },
	{ 0, NULL }
};

const char *aworker_ipc_msg_type_name(uint8_t msg_type)
{
	return get_value_string(msg_type_names, msg_type);
}

/***********************************************************************
 * Message type properties
 ***********************************************************************/

/* 0: unknown type; else the role that may receive it (0 for both) via *receiver */
static bool type_known(uint8_t t, int *receiver)
{
	switch (t) {
	case AWORKER_IPC_MT_HELLO:
	case AWORKER_IPC_MT_ECHO_REQ:
	case AWORKER_IPC_MT_ECHO_RSP:
	case AWORKER_IPC_MT_GOODBYE:
		*receiver = 0;
		return true;
	case AWORKER_IPC_MT_USER_BIND:
	case AWORKER_IPC_MT_USER_UNBIND:
	case AWORKER_IPC_MT_TX_UNITDATA:
	case AWORKER_IPC_MT_TX_CONN_REQ:
	case AWORKER_IPC_MT_TX_CONN_RESP:
	case AWORKER_IPC_MT_TX_DATA:
	case AWORKER_IPC_MT_TX_DISCONN:
		*receiver = AWORKER_IPC_ROLE_WORKER;
		return true;
	case AWORKER_IPC_MT_CS7_INFO:
	case AWORKER_IPC_MT_USER_BIND_RESULT:
	case AWORKER_IPC_MT_SCU_UP:
	case AWORKER_IPC_MT_TX_FAILED:
		*receiver = AWORKER_IPC_ROLE_MAIN;
		return true;
	default:
		return false;
	}
}

/* Messages that are scoped to the link or the worker as a whole, and therefore carry user_ref 0. All others are
 * scoped to one SCCP User and carry its non-zero user_ref. */
static bool type_has_no_user_ref(uint8_t t)
{
	return t == AWORKER_IPC_MT_HELLO || t == AWORKER_IPC_MT_ECHO_REQ || t == AWORKER_IPC_MT_ECHO_RSP
	       || t == AWORKER_IPC_MT_GOODBYE || t == AWORKER_IPC_MT_CS7_INFO;
}

static bool type_is_tx(uint8_t t)
{
	return t >= AWORKER_IPC_MT_TX_UNITDATA && t <= AWORKER_IPC_MT_TX_DISCONN;
}

bool aworker_ipc_type_valid_for(uint8_t msg_type, enum aworker_ipc_role receiver)
{
	int r;
	if (!type_known(msg_type, &r))
		return false;
	return r == 0 || r == (int)receiver;
}

int aworker_ipc_sccp_max_data(uint8_t tx_msg_type)
{
	switch (tx_msg_type) {
	case AWORKER_IPC_MT_TX_UNITDATA:
		return UNITDATA_MAX;
	case AWORKER_IPC_MT_TX_CONN_REQ:
	case AWORKER_IPC_MT_TX_CONN_RESP:
	case AWORKER_IPC_MT_TX_DATA:
		return CO_DATA_MAX;
	case AWORKER_IPC_MT_TX_DISCONN:
		return 0;
	default:
		return -EINVAL;
	}
}

int aworker_ipc_sccp_check_data(uint8_t tx_msg_type, unsigned int len)
{
	int max = aworker_ipc_sccp_max_data(tx_msg_type);
	if (max < 0)
		return max;
	return len > (unsigned int)max ? -EMSGSIZE : 0;
}

/***********************************************************************
 * Field validation shared by encoder and decoder
 ***********************************************************************/

static bool printable(const char *s, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		if (s[i] < 0x20 || s[i] > 0x7e)
			return false;
	}
	return true;
}

/* 0 or AWORKER_IPC_ERR_BAD_ADDR */
static int addr_check(const struct osmo_sccp_addr *a)
{
	if (a->presence & ~OSMO_SCCP_ADDR_T_MASK)
		return AWORKER_IPC_ERR_BAD_ADDR;
	if ((unsigned int)a->ri > OSMO_SCCP_RI_SSN_IP)
		return AWORKER_IPC_ERR_BAD_ADDR;
	if (a->gt.gti > OSMO_SCCP_GTI_TT_NPL_ENC_NAI)
		return AWORKER_IPC_ERR_BAD_ADDR;
	if (strnlen(a->gt.digits, sizeof(a->gt.digits)) >= sizeof(a->gt.digits))
		return AWORKER_IPC_ERR_BAD_ADDR;
	if (a->pc > 0x00ffffff)
		return AWORKER_IPC_ERR_BAD_ADDR;
	if (a->ssn > 255)
		return AWORKER_IPC_ERR_BAD_ADDR;
	return 0;
}

static bool scu_up_allowed(unsigned int primitive, unsigned int operation, bool *data_allowed)
{
	*data_allowed = true;
	switch (OSMO_PRIM(primitive, operation)) {
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_CONFIRM):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_UNITDATA, PRIM_OP_INDICATION):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_NOTICE, PRIM_OP_INDICATION):
		return true;
	case OSMO_PRIM(OSMO_SCU_PRIM_N_STATE, PRIM_OP_INDICATION):
	case OSMO_PRIM(OSMO_SCU_PRIM_N_PCSTATE, PRIM_OP_INDICATION):
		*data_allowed = false;
		return true;
	default:
		return false;
	}
}

/* 0 or an AWORKER_IPC_ERR_* value */
static int scu_up_params_check(const struct osmo_scu_prim *p)
{
	int rc;
	switch (p->oph.primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
		if ((rc = addr_check(&p->u.connect.called_addr)) || (rc = addr_check(&p->u.connect.calling_addr))
		    || (rc = addr_check(&p->u.connect.responding_addr)))
			return rc;
		if (p->u.connect.conn_id > AWORKER_IPC_CONN_ID_MAX)
			return AWORKER_IPC_ERR_BAD_CONN_ID;
		return 0;
	case OSMO_SCU_PRIM_N_DATA:
		if (p->u.data.conn_id > AWORKER_IPC_CONN_ID_MAX)
			return AWORKER_IPC_ERR_BAD_CONN_ID;
		return 0;
	case OSMO_SCU_PRIM_N_DISCONNECT:
		if ((unsigned int)p->u.disconnect.originator > OSMO_SCCP_ORIG_UNDEFINED)
			return AWORKER_IPC_ERR_BAD_VALUE;
		if ((rc = addr_check(&p->u.disconnect.responding_addr)))
			return rc;
		if (p->u.disconnect.conn_id > AWORKER_IPC_CONN_ID_MAX)
			return AWORKER_IPC_ERR_BAD_CONN_ID;
		return 0;
	case OSMO_SCU_PRIM_N_UNITDATA:
		if ((rc = addr_check(&p->u.unitdata.called_addr)) || (rc = addr_check(&p->u.unitdata.calling_addr)))
			return rc;
		return 0;
	case OSMO_SCU_PRIM_N_NOTICE:
		if ((rc = addr_check(&p->u.notice.called_addr)) || (rc = addr_check(&p->u.notice.calling_addr)))
			return rc;
		return 0;
	case OSMO_SCU_PRIM_N_STATE:
	case OSMO_SCU_PRIM_N_PCSTATE:
		return 0;
	default:
		return AWORKER_IPC_ERR_BAD_PRIM;
	}
}

/* 0 or an AWORKER_IPC_ERR_* value */
static int cs7_info_check(const struct aworker_ipc_cs7_info *info)
{
	unsigned int i, j;

	if (info->count < 1 || info->count > AWORKER_IPC_MAX_CS7)
		return AWORKER_IPC_ERR_BAD_VALUE;
	for (i = 0; i < info->count; i++) {
		const struct aworker_ipc_cs7_entry *e = &info->e[i];
		if (e->primary_pc > 0x00ffffff)
			return AWORKER_IPC_ERR_BAD_VALUE;
		if (e->asp_proto < OSMO_SS7_ASP_PROT_SUA || e->asp_proto >= _NUM_OSMO_SS7_ASP_PROT)
			return AWORKER_IPC_ERR_BAD_VALUE;
		if (e->num_as < 1)
			return AWORKER_IPC_ERR_BAD_VALUE;
		/* A cs7 instance is owned by exactly one worker and appears at most once in its report. */
		for (j = 0; j < i; j++) {
			if (info->e[j].cs7_id == e->cs7_id)
				return AWORKER_IPC_ERR_BAD_VALUE;
		}
	}
	return 0;
}

/* 0 or an AWORKER_IPC_ERR_* value */
static int tx_failed_check(const struct aworker_ipc_tx_failed *f)
{
	if (!type_is_tx(f->ref_type))
		return AWORKER_IPC_ERR_BAD_VALUE;
	if (f->ref_type == AWORKER_IPC_MT_TX_UNITDATA) {
		if (f->conn_id != AWORKER_IPC_CONN_ID_NONE)
			return AWORKER_IPC_ERR_BAD_CONN_ID;
	} else if (f->conn_id > AWORKER_IPC_CONN_ID_MAX) {
		return AWORKER_IPC_ERR_BAD_CONN_ID;
	}

	switch (f->disposition) {
	case AWORKER_IPC_TXF_NOT_EXECUTED:
		if (f->lib_rc != 0 || f->post_conn != AWORKER_IPC_POST_CONN_NA)
			return AWORKER_IPC_ERR_BAD_VALUE;
		switch (f->code) {
		case AWORKER_IPC_TXF_NO_SUCH_USER:
			return 0;
		case AWORKER_IPC_TXF_CONN_ID_IN_USE:
			return f->ref_type == AWORKER_IPC_MT_TX_CONN_REQ ? 0 : AWORKER_IPC_ERR_BAD_VALUE;
		case AWORKER_IPC_TXF_CONN_UNKNOWN:
			return (f->ref_type == AWORKER_IPC_MT_TX_CONN_RESP || f->ref_type == AWORKER_IPC_MT_TX_DATA
				|| f->ref_type == AWORKER_IPC_MT_TX_DISCONN) ? 0 : AWORKER_IPC_ERR_BAD_VALUE;
		case AWORKER_IPC_TXF_DATA_TOO_LONG:
			return f->ref_type != AWORKER_IPC_MT_TX_DISCONN ? 0 : AWORKER_IPC_ERR_BAD_VALUE;
		default:
			return AWORKER_IPC_ERR_BAD_VALUE;
		}
	case AWORKER_IPC_TXF_EXECUTED_ERROR:
		if (f->code != AWORKER_IPC_TXF_LIB_ERROR || f->lib_rc >= 0)
			return AWORKER_IPC_ERR_BAD_VALUE;
		if (f->post_conn > AWORKER_IPC_POST_CONN_ABSENT)
			return AWORKER_IPC_ERR_BAD_VALUE;
		if (f->ref_type == AWORKER_IPC_MT_TX_UNITDATA && f->post_conn != AWORKER_IPC_POST_CONN_NA)
			return AWORKER_IPC_ERR_BAD_VALUE;
		return 0;
	default:
		return AWORKER_IPC_ERR_BAD_VALUE;
	}
}

/***********************************************************************
 * Encoder
 ***********************************************************************/

static void put_u8(struct msgb *msg, uint8_t v)
{
	*msgb_put(msg, 1) = v;
}

static void put_u16(struct msgb *msg, uint16_t v)
{
	osmo_store16be(v, msgb_put(msg, 2));
}

static void put_u32(struct msgb *msg, uint32_t v)
{
	osmo_store32be(v, msgb_put(msg, 4));
}

static void put_bytes(struct msgb *msg, const void *buf, size_t len)
{
	if (len)
		memcpy(msgb_put(msg, len), buf, len);
}

/* Caller has checked the address with addr_check(); NULL encodes an absent address. */
static void put_addr(struct msgb *msg, const struct osmo_sccp_addr *a)
{
	uint8_t digits[AWORKER_IPC_DIGITS_LEN] = {};
	uint8_t ip[16] = {};
	size_t dlen;

	if (!a) {
		memset(msgb_put(msg, AWORKER_IPC_ADDR_LEN), 0, AWORKER_IPC_ADDR_LEN);
		return;
	}

	dlen = strnlen(a->gt.digits, sizeof(a->gt.digits));
	memcpy(digits, a->gt.digits, dlen);
	if (a->presence & OSMO_SCCP_ADDR_T_IPv6)
		memcpy(ip, &a->ip.v6, 16);
	else if (a->presence & OSMO_SCCP_ADDR_T_IPv4)
		memcpy(ip, &a->ip.v4, 4);

	put_u32(msg, a->presence);
	put_u8(msg, (uint8_t)a->ri);
	put_u8(msg, a->gt.gti);
	put_u8(msg, a->gt.tt);
	put_u32(msg, a->gt.npi);
	put_u32(msg, a->gt.nai);
	put_u8(msg, (uint8_t)dlen);
	put_bytes(msg, digits, sizeof(digits));
	put_u32(msg, a->pc);
	put_u32(msg, a->ssn);
	put_bytes(msg, ip, sizeof(ip));
}

static void put_data(struct msgb *msg, const uint8_t *data, unsigned int len)
{
	put_u16(msg, (uint16_t)len);
	put_bytes(msg, data, len);
}

static struct msgb *frame_start(void *ctx, uint8_t type, uint32_t user_ref)
{
	struct msgb *msg = msgb_alloc_c(ctx, AWORKER_IPC_MAX_FRAME, "aworker-ipc frame");
	if (!msg)
		return NULL;
	put_u8(msg, AWORKER_IPC_MAGIC0);
	put_u8(msg, AWORKER_IPC_MAGIC1);
	put_u8(msg, AWORKER_IPC_VERSION);
	put_u8(msg, type);
	put_u16(msg, 0);	/* flags */
	put_u16(msg, 0);	/* body_len, set by frame_end() */
	put_u32(msg, 0);	/* seq, set by the link */
	put_u32(msg, user_ref);
	return msg;
}

static int frame_end(struct msgb *msg, struct msgb **out)
{
	OSMO_ASSERT(msgb_length(msg) >= AWORKER_IPC_HDR_LEN && msgb_length(msg) <= AWORKER_IPC_MAX_FRAME);
	osmo_store16be(msgb_length(msg) - AWORKER_IPC_HDR_LEN, msgb_data(msg) + 6);
	*out = msg;
	return 0;
}

static int check_user_ref(uint32_t user_ref)
{
	return user_ref ? 0 : -EINVAL;
}

int aworker_ipc_enc_hello(void *ctx, struct msgb **out, enum aworker_ipc_role role, uint16_t worker_idx,
			  uint32_t pid, const char *ident)
{
	struct msgb *msg;
	size_t ilen = ident ? strlen(ident) : 0;

	*out = NULL;
	if (role != AWORKER_IPC_ROLE_MAIN && role != AWORKER_IPC_ROLE_WORKER)
		return -EINVAL;
	if (ilen > AWORKER_IPC_MAX_IDENT || !printable(ident, ilen))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_HELLO, 0)))
		return -ENOMEM;
	put_u8(msg, role);
	put_u8(msg, 0);
	put_u16(msg, worker_idx);
	put_u16(msg, AWORKER_IPC_MAX_FRAME);
	put_u16(msg, AWORKER_IPC_MAX_DATA);
	put_u32(msg, pid);
	put_u8(msg, (uint8_t)ilen);
	put_bytes(msg, ident, ilen);
	return frame_end(msg, out);
}

int aworker_ipc_enc_echo(void *ctx, struct msgb **out, bool response, uint32_t token)
{
	struct msgb *msg;

	*out = NULL;
	if (!(msg = frame_start(ctx, response ? AWORKER_IPC_MT_ECHO_RSP : AWORKER_IPC_MT_ECHO_REQ, 0)))
		return -ENOMEM;
	put_u32(msg, token);
	return frame_end(msg, out);
}

int aworker_ipc_enc_goodbye(void *ctx, struct msgb **out, uint8_t reason)
{
	struct msgb *msg;

	*out = NULL;
	if (reason != AWORKER_IPC_GOODBYE_SHUTDOWN)
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_GOODBYE, 0)))
		return -ENOMEM;
	put_u8(msg, reason);
	return frame_end(msg, out);
}

int aworker_ipc_enc_cs7_info(void *ctx, struct msgb **out, const struct aworker_ipc_cs7_info *info)
{
	struct msgb *msg;
	unsigned int i;

	*out = NULL;
	if (!info || cs7_info_check(info))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_CS7_INFO, 0)))
		return -ENOMEM;
	put_u8(msg, info->count);
	for (i = 0; i < info->count; i++) {
		const struct aworker_ipc_cs7_entry *e = &info->e[i];
		put_u32(msg, e->cs7_id);
		put_u32(msg, e->primary_pc);
		put_u8(msg, e->asp_proto);
		put_u8(msg, e->num_as);
		put_u8(msg, 0);
		put_u8(msg, 0);
	}
	return frame_end(msg, out);
}

int aworker_ipc_enc_user_bind(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t cs7_id, uint16_t ssn,
			      const char *name)
{
	struct msgb *msg;
	size_t nlen = name ? strlen(name) : 0;

	*out = NULL;
	if (check_user_ref(user_ref) || ssn > 255)
		return -EINVAL;
	if (nlen < 1 || nlen > AWORKER_IPC_MAX_USER_NAME || !printable(name, nlen))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_USER_BIND, user_ref)))
		return -ENOMEM;
	put_u32(msg, cs7_id);
	put_u16(msg, ssn);
	put_u8(msg, (uint8_t)nlen);
	put_bytes(msg, name, nlen);
	return frame_end(msg, out);
}

int aworker_ipc_enc_user_unbind(void *ctx, struct msgb **out, uint32_t user_ref)
{
	struct msgb *msg;

	*out = NULL;
	if (check_user_ref(user_ref))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_USER_UNBIND, user_ref)))
		return -ENOMEM;
	return frame_end(msg, out);
}

int aworker_ipc_enc_user_bind_result(void *ctx, struct msgb **out, uint32_t user_ref, uint16_t result)
{
	struct msgb *msg;

	*out = NULL;
	if (check_user_ref(user_ref) || result > AWORKER_IPC_BIND_ALREADY_BOUND)
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_USER_BIND_RESULT, user_ref)))
		return -ENOMEM;
	put_u16(msg, result);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_unitdata(void *ctx, struct msgb **out, uint32_t user_ref,
				const struct osmo_sccp_addr *calling_addr, const struct osmo_sccp_addr *called_addr,
				struct msgb *data)
{
	struct msgb *msg;
	int rc = 0;

	*out = NULL;
	if (!data)
		return -EINVAL;
	if (check_user_ref(user_ref) || !calling_addr || !called_addr
	    || addr_check(calling_addr) || addr_check(called_addr))
		rc = -EINVAL;
	else if (msgb_length(data) > AWORKER_IPC_MAX_DATA)
		rc = -EMSGSIZE;
	else if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_UNITDATA, user_ref)))
		rc = -ENOMEM;
	if (rc) {
		msgb_free(data);
		return rc;
	}
	put_addr(msg, calling_addr);
	put_addr(msg, called_addr);
	put_data(msg, msgb_data(data), msgb_length(data));
	msgb_free(data);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_conn_req(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
				const struct osmo_sccp_addr *calling_addr, const struct osmo_sccp_addr *called_addr,
				struct msgb *data)
{
	struct msgb *msg;
	int rc = 0;

	*out = NULL;
	if (!data)
		return -EINVAL;
	if (check_user_ref(user_ref) || conn_id > AWORKER_IPC_CONN_ID_MAX || !called_addr
	    || (calling_addr && addr_check(calling_addr)) || addr_check(called_addr))
		rc = -EINVAL;
	else if (msgb_length(data) > AWORKER_IPC_MAX_DATA)
		rc = -EMSGSIZE;
	else if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_CONN_REQ, user_ref)))
		rc = -ENOMEM;
	if (rc) {
		msgb_free(data);
		return rc;
	}
	put_u32(msg, conn_id);
	put_u8(msg, calling_addr ? FLAG_ADDR_PRESENT : 0);
	put_addr(msg, calling_addr);
	put_addr(msg, called_addr);
	put_data(msg, msgb_data(data), msgb_length(data));
	msgb_free(data);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_conn_resp(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
				 const struct osmo_sccp_addr *resp_addr, const uint8_t *data, unsigned int len)
{
	struct msgb *msg;

	*out = NULL;
	/* osmo_sccp_tx_conn_resp_msg() dereferences resp_addr unconditionally, so it is mandatory here. */
	if (check_user_ref(user_ref) || conn_id > AWORKER_IPC_CONN_ID_MAX || !resp_addr || addr_check(resp_addr))
		return -EINVAL;
	if (len && !data)
		return -EINVAL;
	if (len > AWORKER_IPC_MAX_DATA)
		return -EMSGSIZE;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_CONN_RESP, user_ref)))
		return -ENOMEM;
	put_u32(msg, conn_id);
	put_addr(msg, resp_addr);
	put_data(msg, data, len);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_data(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id, struct msgb *data)
{
	struct msgb *msg;
	int rc = 0;

	*out = NULL;
	if (!data)
		return -EINVAL;
	if (check_user_ref(user_ref) || conn_id > AWORKER_IPC_CONN_ID_MAX)
		rc = -EINVAL;
	else if (msgb_length(data) > AWORKER_IPC_MAX_DATA)
		rc = -EMSGSIZE;
	else if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_DATA, user_ref)))
		rc = -ENOMEM;
	if (rc) {
		msgb_free(data);
		return rc;
	}
	put_u32(msg, conn_id);
	put_data(msg, msgb_data(data), msgb_length(data));
	msgb_free(data);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_disconn(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
			       const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	struct msgb *msg;

	*out = NULL;
	if (check_user_ref(user_ref) || conn_id > AWORKER_IPC_CONN_ID_MAX || (resp_addr && addr_check(resp_addr)))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_DISCONN, user_ref)))
		return -ENOMEM;
	put_u32(msg, conn_id);
	put_u8(msg, resp_addr ? FLAG_ADDR_PRESENT : 0);
	put_addr(msg, resp_addr);
	put_u32(msg, cause);
	return frame_end(msg, out);
}

int aworker_ipc_enc_scu_up(void *ctx, struct msgb **out, uint32_t user_ref, const struct osmo_scu_prim *prim,
			   const uint8_t *data, unsigned int len)
{
	const struct osmo_prim_hdr *oph;
	struct msgb *msg;
	bool data_allowed;

	*out = NULL;
	if (check_user_ref(user_ref) || !prim)
		return -EINVAL;
	oph = &prim->oph;
	if (oph->sap != SCCP_SAP_USER || !scu_up_allowed(oph->primitive, oph->operation, &data_allowed))
		return -ENOTSUP;
	if (len && !data)
		return -EINVAL;
	if (len && !data_allowed)
		return -EINVAL;
	if (len > AWORKER_IPC_MAX_DATA)
		return -EMSGSIZE;
	if (scu_up_params_check(prim))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_SCU_UP, user_ref)))
		return -ENOMEM;

	put_u8(msg, (uint8_t)oph->primitive);
	put_u8(msg, (uint8_t)oph->operation);
	put_u16(msg, 0);
	switch (oph->primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
		put_addr(msg, &prim->u.connect.called_addr);
		put_addr(msg, &prim->u.connect.calling_addr);
		put_addr(msg, &prim->u.connect.responding_addr);
		put_u32(msg, prim->u.connect.sccp_class);
		put_u32(msg, prim->u.connect.importance);
		put_u32(msg, prim->u.connect.conn_id);
		break;
	case OSMO_SCU_PRIM_N_DATA:
		put_u32(msg, prim->u.data.conn_id);
		put_u32(msg, prim->u.data.importance);
		break;
	case OSMO_SCU_PRIM_N_DISCONNECT:
		put_u32(msg, prim->u.disconnect.originator);
		put_addr(msg, &prim->u.disconnect.responding_addr);
		put_u32(msg, prim->u.disconnect.cause);
		put_u32(msg, prim->u.disconnect.conn_id);
		put_u32(msg, prim->u.disconnect.importance);
		break;
	case OSMO_SCU_PRIM_N_UNITDATA:
		put_addr(msg, &prim->u.unitdata.called_addr);
		put_addr(msg, &prim->u.unitdata.calling_addr);
		put_u32(msg, prim->u.unitdata.in_sequence_control);
		put_u32(msg, prim->u.unitdata.return_option);
		put_u32(msg, prim->u.unitdata.importance);
		break;
	case OSMO_SCU_PRIM_N_NOTICE:
		put_addr(msg, &prim->u.notice.called_addr);
		put_addr(msg, &prim->u.notice.calling_addr);
		put_u32(msg, prim->u.notice.cause);
		put_u32(msg, prim->u.notice.importance);
		break;
	case OSMO_SCU_PRIM_N_STATE:
		put_u32(msg, prim->u.state.affected_pc);
		put_u32(msg, prim->u.state.affected_ssn);
		put_u8(msg, prim->u.state.user_in_service ? 1 : 0);
		put_u32(msg, prim->u.state.ssn_multiplicity_ind);
		break;
	case OSMO_SCU_PRIM_N_PCSTATE:
		put_u32(msg, prim->u.pcstate.affected_pc);
		put_u32(msg, prim->u.pcstate.restricted_importance_level);
		put_u32(msg, prim->u.pcstate.sp_status);
		put_u32(msg, prim->u.pcstate.remote_sccp_status);
		break;
	default:
		OSMO_ASSERT(0);
	}
	put_data(msg, data, len);
	return frame_end(msg, out);
}

int aworker_ipc_enc_tx_failed(void *ctx, struct msgb **out, uint32_t user_ref, const struct aworker_ipc_tx_failed *f)
{
	struct msgb *msg;

	*out = NULL;
	if (check_user_ref(user_ref) || !f || tx_failed_check(f))
		return -EINVAL;
	if (!(msg = frame_start(ctx, AWORKER_IPC_MT_TX_FAILED, user_ref)))
		return -ENOMEM;
	put_u32(msg, f->ref_seq);
	put_u8(msg, f->ref_type);
	put_u8(msg, f->disposition);
	put_u16(msg, f->code);
	put_u32(msg, f->conn_id);
	put_u32(msg, (uint32_t)f->lib_rc);
	put_u8(msg, f->post_conn);
	put_u8(msg, 0);
	put_u8(msg, 0);
	put_u8(msg, 0);
	return frame_end(msg, out);
}

/***********************************************************************
 * Decoder
 ***********************************************************************/

struct rd {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	int err;	/* first error (positive AWORKER_IPC_ERR_*), sticky */
};

static void rd_fail(struct rd *r, int err)
{
	if (!r->err)
		r->err = err;
}

static const uint8_t *rd_take(struct rd *r, size_t n)
{
	const uint8_t *p;
	if (r->err)
		return NULL;
	if (r->len - r->pos < n) {
		rd_fail(r, AWORKER_IPC_ERR_TRUNCATED);
		return NULL;
	}
	p = r->buf + r->pos;
	r->pos += n;
	return p;
}

static uint8_t get_u8(struct rd *r)
{
	const uint8_t *p = rd_take(r, 1);
	return p ? p[0] : 0;
}

static uint16_t get_u16(struct rd *r)
{
	const uint8_t *p = rd_take(r, 2);
	return p ? osmo_load16be(p) : 0;
}

static uint32_t get_u32(struct rd *r)
{
	const uint8_t *p = rd_take(r, 4);
	return p ? osmo_load32be(p) : 0;
}

static bool all_zero(const uint8_t *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		if (p[i])
			return false;
	}
	return true;
}

static void get_addr(struct rd *r, struct osmo_sccp_addr *a)
{
	const uint8_t *digits, *ip;
	uint8_t dlen, ri;
	unsigned int i;

	memset(a, 0, sizeof(*a));
	a->presence = get_u32(r);
	ri = get_u8(r);
	a->gt.gti = get_u8(r);
	a->gt.tt = get_u8(r);
	a->gt.npi = get_u32(r);
	a->gt.nai = get_u32(r);
	dlen = get_u8(r);
	digits = rd_take(r, AWORKER_IPC_DIGITS_LEN);
	a->pc = get_u32(r);
	a->ssn = get_u32(r);
	ip = rd_take(r, 16);
	if (r->err)
		return;

	if (ri > OSMO_SCCP_RI_SSN_IP || dlen >= AWORKER_IPC_DIGITS_LEN) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
		return;
	}
	a->ri = ri;
	for (i = 0; i < dlen; i++) {
		if (!digits[i]) {
			rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
			return;
		}
	}
	if (!all_zero(digits + dlen, AWORKER_IPC_DIGITS_LEN - dlen)) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
		return;
	}
	memcpy(a->gt.digits, digits, dlen);

	if (a->presence & OSMO_SCCP_ADDR_T_IPv6) {
		memcpy(&a->ip.v6, ip, 16);
	} else if (a->presence & OSMO_SCCP_ADDR_T_IPv4) {
		if (!all_zero(ip + 4, 12)) {
			rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
			return;
		}
		memcpy(&a->ip.v4, ip, 4);
	} else if (!all_zero(ip, 16)) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
		return;
	}

	if (addr_check(a))
		rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
}

/* An optional address preceded by its presence flag octet (already read). */
static void get_opt_addr(struct rd *r, uint8_t flags, bool *present, struct osmo_sccp_addr *a)
{
	const uint8_t *raw;

	if (flags & ~FLAG_ADDR_PRESENT) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_RESERVED);
		return;
	}
	*present = !!(flags & FLAG_ADDR_PRESENT);
	if (*present) {
		get_addr(r, a);
		return;
	}
	memset(a, 0, sizeof(*a));
	raw = rd_take(r, AWORKER_IPC_ADDR_LEN);
	if (raw && !all_zero(raw, AWORKER_IPC_ADDR_LEN))
		rd_fail(r, AWORKER_IPC_ERR_BAD_ADDR);
}

static uint32_t get_conn_id(struct rd *r)
{
	uint32_t conn_id = get_u32(r);
	if (!r->err && conn_id > AWORKER_IPC_CONN_ID_MAX)
		rd_fail(r, AWORKER_IPC_ERR_BAD_CONN_ID);
	return conn_id;
}

static void get_data(struct rd *r, struct aworker_ipc_data *d)
{
	uint16_t len = get_u16(r);
	if (r->err)
		return;
	if (len > AWORKER_IPC_MAX_DATA) {
		rd_fail(r, AWORKER_IPC_ERR_DATA_TOO_LONG);
		return;
	}
	d->buf = rd_take(r, len);
	d->len = r->err ? 0 : len;
}

/* Fixed-length printable string preceded by its u8 length */
static void get_string(struct rd *r, char *dst, size_t min_len, size_t max_len)
{
	uint8_t len = get_u8(r);
	const uint8_t *p;

	if (r->err)
		return;
	if (len < min_len || len > max_len) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_STRING);
		return;
	}
	p = rd_take(r, len);
	if (!p)
		return;
	if (!printable((const char *)p, len)) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_STRING);
		return;
	}
	memcpy(dst, p, len);
	dst[len] = '\0';
}

static void dec_scu_up(struct rd *r, struct aworker_ipc_msg *m)
{
	struct osmo_scu_prim *p = &m->u.scu_up;
	uint8_t primitive, operation, uis;
	bool data_allowed;
	int rc;

	primitive = get_u8(r);
	operation = get_u8(r);
	if (get_u16(r) != 0)
		rd_fail(r, AWORKER_IPC_ERR_BAD_RESERVED);
	if (r->err)
		return;
	if (!scu_up_allowed(primitive, operation, &data_allowed)) {
		rd_fail(r, AWORKER_IPC_ERR_BAD_PRIM);
		return;
	}
	osmo_prim_init(&p->oph, SCCP_SAP_USER, primitive, operation, NULL);

	switch (primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
		get_addr(r, &p->u.connect.called_addr);
		get_addr(r, &p->u.connect.calling_addr);
		get_addr(r, &p->u.connect.responding_addr);
		p->u.connect.sccp_class = get_u32(r);
		p->u.connect.importance = get_u32(r);
		p->u.connect.conn_id = get_conn_id(r);
		break;
	case OSMO_SCU_PRIM_N_DATA:
		p->u.data.conn_id = get_conn_id(r);
		p->u.data.importance = get_u32(r);
		break;
	case OSMO_SCU_PRIM_N_DISCONNECT:
		p->u.disconnect.originator = get_u32(r);
		get_addr(r, &p->u.disconnect.responding_addr);
		p->u.disconnect.cause = get_u32(r);
		p->u.disconnect.conn_id = get_conn_id(r);
		p->u.disconnect.importance = get_u32(r);
		break;
	case OSMO_SCU_PRIM_N_UNITDATA:
		get_addr(r, &p->u.unitdata.called_addr);
		get_addr(r, &p->u.unitdata.calling_addr);
		p->u.unitdata.in_sequence_control = get_u32(r);
		p->u.unitdata.return_option = get_u32(r);
		p->u.unitdata.importance = get_u32(r);
		break;
	case OSMO_SCU_PRIM_N_NOTICE:
		get_addr(r, &p->u.notice.called_addr);
		get_addr(r, &p->u.notice.calling_addr);
		p->u.notice.cause = get_u32(r);
		p->u.notice.importance = get_u32(r);
		break;
	case OSMO_SCU_PRIM_N_STATE:
		p->u.state.affected_pc = get_u32(r);
		p->u.state.affected_ssn = get_u32(r);
		uis = get_u8(r);
		if (!r->err && uis > 1)
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
		p->u.state.user_in_service = uis == 1;
		p->u.state.ssn_multiplicity_ind = get_u32(r);
		break;
	case OSMO_SCU_PRIM_N_PCSTATE:
		p->u.pcstate.affected_pc = get_u32(r);
		p->u.pcstate.restricted_importance_level = get_u32(r);
		p->u.pcstate.sp_status = get_u32(r);
		p->u.pcstate.remote_sccp_status = get_u32(r);
		break;
	default:
		OSMO_ASSERT(0);
	}
	if (r->err)
		return;
	if ((rc = scu_up_params_check(p))) {
		rd_fail(r, rc);
		return;
	}
	get_data(r, &m->data);
	if (!r->err && m->data.len && !data_allowed)
		rd_fail(r, AWORKER_IPC_ERR_UNEXPECTED_DATA);
}

static void dec_body(struct rd *r, struct aworker_ipc_msg *m)
{
	uint8_t u8;
	int rc;

	switch (m->type) {
	case AWORKER_IPC_MT_HELLO:
		m->u.hello.role = get_u8(r);
		if (get_u8(r) != 0)
			rd_fail(r, AWORKER_IPC_ERR_BAD_RESERVED);
		m->u.hello.worker_idx = get_u16(r);
		m->u.hello.max_frame = get_u16(r);
		m->u.hello.max_data = get_u16(r);
		m->u.hello.pid = get_u32(r);
		if (!r->err && m->u.hello.role != AWORKER_IPC_ROLE_MAIN && m->u.hello.role != AWORKER_IPC_ROLE_WORKER)
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
		get_string(r, m->u.hello.ident, 0, AWORKER_IPC_MAX_IDENT);
		break;
	case AWORKER_IPC_MT_ECHO_REQ:
	case AWORKER_IPC_MT_ECHO_RSP:
		m->u.echo_token = get_u32(r);
		break;
	case AWORKER_IPC_MT_GOODBYE:
		m->u.goodbye_reason = get_u8(r);
		if (!r->err && m->u.goodbye_reason != AWORKER_IPC_GOODBYE_SHUTDOWN)
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
		break;
	case AWORKER_IPC_MT_CS7_INFO: {
		unsigned int i;
		m->u.cs7_info.count = get_u8(r);
		if (r->err)
			break;
		if (m->u.cs7_info.count < 1 || m->u.cs7_info.count > AWORKER_IPC_MAX_CS7) {
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
			break;
		}
		for (i = 0; i < m->u.cs7_info.count; i++) {
			struct aworker_ipc_cs7_entry *e = &m->u.cs7_info.e[i];
			e->cs7_id = get_u32(r);
			e->primary_pc = get_u32(r);
			e->asp_proto = get_u8(r);
			e->num_as = get_u8(r);
			if (get_u8(r) != 0 || get_u8(r) != 0)
				rd_fail(r, AWORKER_IPC_ERR_BAD_RESERVED);
		}
		if (!r->err && (rc = cs7_info_check(&m->u.cs7_info)))
			rd_fail(r, rc);
		break;
	}
	case AWORKER_IPC_MT_USER_BIND:
		m->u.user_bind.cs7_id = get_u32(r);
		m->u.user_bind.ssn = get_u16(r);
		if (!r->err && m->u.user_bind.ssn > 255)
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
		get_string(r, m->u.user_bind.name, 1, AWORKER_IPC_MAX_USER_NAME);
		break;
	case AWORKER_IPC_MT_USER_UNBIND:
		break;
	case AWORKER_IPC_MT_USER_BIND_RESULT:
		m->u.bind_result = get_u16(r);
		if (!r->err && m->u.bind_result > AWORKER_IPC_BIND_ALREADY_BOUND)
			rd_fail(r, AWORKER_IPC_ERR_BAD_VALUE);
		break;
	case AWORKER_IPC_MT_TX_UNITDATA:
		get_addr(r, &m->u.tx_unitdata.calling);
		get_addr(r, &m->u.tx_unitdata.called);
		get_data(r, &m->data);
		break;
	case AWORKER_IPC_MT_TX_CONN_REQ:
		m->u.tx_conn_req.conn_id = get_conn_id(r);
		u8 = get_u8(r);
		if (!r->err)
			get_opt_addr(r, u8, &m->u.tx_conn_req.calling_present, &m->u.tx_conn_req.calling);
		get_addr(r, &m->u.tx_conn_req.called);
		get_data(r, &m->data);
		break;
	case AWORKER_IPC_MT_TX_CONN_RESP:
		m->u.tx_conn_resp.conn_id = get_conn_id(r);
		get_addr(r, &m->u.tx_conn_resp.resp);
		get_data(r, &m->data);
		break;
	case AWORKER_IPC_MT_TX_DATA:
		m->u.tx_data.conn_id = get_conn_id(r);
		get_data(r, &m->data);
		break;
	case AWORKER_IPC_MT_TX_DISCONN:
		m->u.tx_disconn.conn_id = get_conn_id(r);
		u8 = get_u8(r);
		if (!r->err)
			get_opt_addr(r, u8, &m->u.tx_disconn.resp_present, &m->u.tx_disconn.resp);
		m->u.tx_disconn.cause = get_u32(r);
		break;
	case AWORKER_IPC_MT_SCU_UP:
		dec_scu_up(r, m);
		break;
	case AWORKER_IPC_MT_TX_FAILED:
		m->u.tx_failed.ref_seq = get_u32(r);
		m->u.tx_failed.ref_type = get_u8(r);
		m->u.tx_failed.disposition = get_u8(r);
		m->u.tx_failed.code = get_u16(r);
		m->u.tx_failed.conn_id = get_u32(r);
		m->u.tx_failed.lib_rc = (int32_t)get_u32(r);
		m->u.tx_failed.post_conn = get_u8(r);
		if (get_u8(r) != 0 || get_u8(r) != 0 || get_u8(r) != 0)
			rd_fail(r, AWORKER_IPC_ERR_BAD_RESERVED);
		if (!r->err && (rc = tx_failed_check(&m->u.tx_failed)))
			rd_fail(r, rc);
		break;
	default:
		OSMO_ASSERT(0);
	}
}

/*! Decode and fully validate one record.
 * \param[out] out  decoded message; DATA views point into buf.
 * \param[in] buf  the record.
 * \param[in] len  record length.
 * \param[in] receiver  role of the decoding side, for the direction check.
 * \returns 0 on success, or a negative enum aworker_ipc_err value. */
int aworker_ipc_dec(struct aworker_ipc_msg *out, const uint8_t *buf, size_t len, enum aworker_ipc_role receiver)
{
	struct rd r;
	uint16_t body_len;
	int dir;

	memset(out, 0, sizeof(*out));
	if (len < AWORKER_IPC_HDR_LEN)
		return -AWORKER_IPC_ERR_FRAME_TOO_SHORT;
	if (len > AWORKER_IPC_MAX_FRAME)
		return -AWORKER_IPC_ERR_FRAME_TOO_LONG;
	if (buf[0] != AWORKER_IPC_MAGIC0 || buf[1] != AWORKER_IPC_MAGIC1)
		return -AWORKER_IPC_ERR_BAD_MAGIC;
	if (buf[2] != AWORKER_IPC_VERSION)
		return -AWORKER_IPC_ERR_BAD_VERSION;
	if (osmo_load16be(buf + 4) != 0)
		return -AWORKER_IPC_ERR_BAD_FLAGS;
	body_len = osmo_load16be(buf + 6);
	if ((size_t)body_len != len - AWORKER_IPC_HDR_LEN)
		return -AWORKER_IPC_ERR_BAD_BODY_LEN;
	out->type = buf[3];
	if (!type_known(out->type, &dir))
		return -AWORKER_IPC_ERR_UNKNOWN_TYPE;
	if (dir != 0 && dir != (int)receiver)
		return -AWORKER_IPC_ERR_WRONG_DIRECTION;
	out->seq = osmo_load32be(buf + 8);
	out->user_ref = osmo_load32be(buf + 12);
	if (type_has_no_user_ref(out->type) ? out->user_ref != 0 : out->user_ref == 0)
		return -AWORKER_IPC_ERR_BAD_USER_REF;

	r = (struct rd){ .buf = buf + AWORKER_IPC_HDR_LEN, .len = body_len };
	dec_body(&r, out);
	if (!r.err && r.pos != r.len)
		r.err = AWORKER_IPC_ERR_TRAILING_DATA;
	if (r.err) {
		int err = r.err;
		memset(out, 0, sizeof(*out));
		return -err;
	}
	return 0;
}

/*! Build the primitive of a decoded SCU_UP inside a new msgb, laid out as libosmo-sigtran does it: struct
 * osmo_scu_prim at msgb_data(), user data at l2h. The caller owns the msgb (and passes it on to an SCCP user
 * call-back, which frees it). Returns NULL if msg is not an SCU_UP or on allocation failure. */
struct msgb *aworker_ipc_build_scu_prim(void *ctx, const struct aworker_ipc_msg *msg)
{
	const struct osmo_scu_prim *src;
	struct osmo_scu_prim *prim;
	struct msgb *upmsg;
	size_t need;

	if (msg->type != AWORKER_IPC_MT_SCU_UP)
		return NULL;
	src = &msg->u.scu_up;
	need = sizeof(*prim) + msg->data.len;

	switch (src->oph.primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
	case OSMO_SCU_PRIM_N_DATA:
	case OSMO_SCU_PRIM_N_DISCONNECT:
		upmsg = msgb_alloc_c(ctx, OSMO_MAX(need, SIGTRAN_UP_CO_SIZE), "aworker-ipc SCU prim");
		break;
	default:
		upmsg = msgb_alloc_headroom_c(ctx, OSMO_MAX(need, SIGTRAN_UP_CL_SIZE) + SIGTRAN_UP_CL_HEADROOM,
					      SIGTRAN_UP_CL_HEADROOM, "aworker-ipc SCU prim");
		break;
	}
	if (!upmsg)
		return NULL;

	prim = (struct osmo_scu_prim *)msgb_put(upmsg, sizeof(*prim));
	*prim = *src;
	osmo_prim_init(&prim->oph, SCCP_SAP_USER, src->oph.primitive, src->oph.operation, upmsg);
	upmsg->l2h = upmsg->tail;
	if (msg->data.len)
		memcpy(msgb_put(upmsg, msg->data.len), msg->data.buf, msg->data.len);
	return upmsg;
}

uint8_t aworker_ipc_frame_type(const uint8_t *buf)
{
	return buf[3];
}

uint32_t aworker_ipc_frame_seq(const uint8_t *buf)
{
	return osmo_load32be(buf + 8);
}

void aworker_ipc_frame_set_seq(uint8_t *buf, uint32_t seq)
{
	osmo_store32be(seq, buf + 8);
}

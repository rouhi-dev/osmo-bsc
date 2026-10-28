/* A-worker IPC: wire protocol and codec between the osmo-bsc main process and its A-interface (SIGTRAN) workers */
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

/* PROTOCOL (version 1)
 * ====================
 *
 * Transport: one AF_UNIX SOCK_SEQPACKET connection per worker; one record == one message (see aworker_ipc_link.h).
 * All integers are big-endian. No C structs or pointers are put on the wire. Every field is validated on decode;
 * any violation is fatal for the link (fail closed).
 *
 * Common header (AWORKER_IPC_HDR_LEN = 16 octets):
 *   0  u8[2] magic     'A' 'W'
 *   2  u8    version   AWORKER_IPC_VERSION; the only version field of the protocol, exact match required
 *   3  u8    msg_type  enum aworker_ipc_msg_type; must be valid for the direction
 *   4  u16   flags     must be 0
 *   6  u16   body_len  must equal record length - 16
 *   8  u32   seq       per-direction sequence number: HELLO is 1, then +1 per record (mod 2^32)
 *   12 u32   user_ref  0 for link and worker-scoped messages (HELLO, ECHO_*, GOODBYE, CS7_INFO); non-zero
 *                       (assigned by main) for the SCCP-User scoped ones
 *
 * Records are at most AWORKER_IPC_MAX_FRAME octets. DATA blobs carry at most AWORKER_IPC_MAX_DATA octets: this is
 * a transport ceiling only. The SCCP operations have their own, smaller limits (aworker_ipc_sccp_max_data()), which
 * must be checked before calling a libosmo-sigtran helper: larger data panics in the M3UA/IPA encoders, is silently
 * dropped, or is encoded with a wrong length.
 *
 * ADDR (AWORKER_IPC_ADDR_LEN = 72 octets), every field of struct osmo_sccp_addr:
 *   presence u32 (no bits outside OSMO_SCCP_ADDR_T_MASK) | ri u8 (<= OSMO_SCCP_RI_SSN_IP) | gt.gti u8 (<= 4) |
 *   gt.tt u8 | gt.npi u32 | gt.nai u32 | digits_len u8 (<= 31) | digits[32] (no NUL before digits_len, zero after) |
 *   pc u32 (<= 0xffffff) | ssn u32 (<= 255) | ip[16] (raw in_addr/in6_addr bytes; bytes not covered by the IPv4 /
 *   IPv6 presence bits are zero)
 * An absent optional ADDR is encoded as 72 zero octets with its presence flag cleared.
 *
 * DATA: u16 len (<= AWORKER_IPC_MAX_DATA) + len octets; DATA is always the last element and must end the record.
 *
 * Bodies (direction: B = both, M = main->worker, W = worker->main):
 *   HELLO          B  role u8 (1 main, 2 worker) | reserved u8 = 0 | worker_idx u16 | max_frame u16 | max_data u16 |
 *                     pid u32 | ident_len u8 (<= 64) | ident (printable ASCII)
 *   ECHO_REQ/RSP   B  token u32
 *   GOODBYE        B  reason u8 (enum aworker_ipc_goodbye_reason)
 *   CS7_INFO       W  count u8 (1..8) | count * { cs7_id u32 | primary_pc u32 (<= 0xffffff) |
 *                     asp_proto u8 (1..3, enum osmo_ss7_asp_protocol) | num_as u8 (>= 1, Application Servers on
 *                     that instance) | reserved u8[2] = 0 }
 *   USER_BIND      M  cs7_id u32 | ssn u16 (<= 255) | name_len u8 (1..32) | name (printable ASCII)
 *   USER_UNBIND    M  (empty)
 *   USER_BIND_RES  W  result u16 (enum aworker_ipc_bind_result)
 *   TX_UNITDATA    M  calling ADDR | called ADDR | DATA
 *   TX_CONN_REQ    M  conn_id u32 | flags u8 (bit0: calling present) | calling ADDR | called ADDR | DATA
 *   TX_CONN_RESP   M  conn_id u32 | responding ADDR | DATA
 *   TX_DATA        M  conn_id u32 | DATA
 *   TX_DISCONN     M  conn_id u32 | flags u8 (bit0: responding present) | responding ADDR | cause u32
 *   SCU_UP         W  primitive u8 | operation u8 | reserved u16 = 0 | parameters | DATA
 *   TX_FAILED      W  ref_seq u32 | ref_type u8 | disposition u8 | code u16 | conn_id u32 | lib_rc i32 |
 *                     post_conn u8 | reserved u8[3] = 0
 * conn_id is <= AWORKER_IPC_CONN_ID_MAX (24 bit SCCP local reference, all-ones excluded).
 *
 * SCU_UP parameters, per allowed (primitive, operation); every field of the osmo_scu_*_param struct:
 *   N-CONNECT.ind/.cnf  called ADDR | calling ADDR | responding ADDR | sccp_class u32 | importance u32 | conn_id u32
 *   N-DATA.ind          conn_id u32 | importance u32
 *   N-DISCONNECT.ind    originator u32 (<= OSMO_SCCP_ORIG_UNDEFINED) | responding ADDR | cause u32 | conn_id u32 |
 *                       importance u32
 *   N-UNITDATA.ind      called ADDR | calling ADDR | in_sequence_control u32 | return_option u32 | importance u32
 *   N-NOTICE.ind        called ADDR | calling ADDR | cause u32 | importance u32
 *   N-STATE.ind         affected_pc u32 | affected_ssn u32 | user_in_service u8 (0/1) | ssn_multiplicity_ind u32
 *   N-PCSTATE.ind       affected_pc u32 | restricted_importance_level u32 | sp_status u32 | remote_sccp_status u32
 * N-STATE and N-PCSTATE carry an empty DATA. Any other primitive is rejected.
 *
 * TX_FAILED contract: sent by the worker at most once per TX_* request, never on success.
 *   disposition NOT_EXECUTED: the libosmo-sigtran helper was NOT called; nothing was sent towards the network.
 *     code NO_SUCH_USER (any TX), CONN_ID_IN_USE (TX_CONN_REQ only), CONN_UNKNOWN (TX_CONN_RESP, TX_DATA,
 *     TX_DISCONN), DATA_TOO_LONG (TX_UNITDATA, TX_CONN_REQ, TX_CONN_RESP, TX_DATA); lib_rc = 0; post_conn = 0.
 *   disposition EXECUTED_ERROR: the helper was called and returned lib_rc < 0; the effect on the network is unknown.
 *     code LIB_ERROR; post_conn optionally reports whether conn_id exists afterwards (informational only).
 *   conn_id is AWORKER_IPC_CONN_ID_NONE for TX_UNITDATA, else the request's conn_id.
 * A malformed request gets no TX_FAILED: the link is closed instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <osmocom/sigtran/sccp_sap.h>

struct msgb;

#define AWORKER_IPC_MAGIC0		0x41	/* 'A' */
#define AWORKER_IPC_MAGIC1		0x57	/* 'W' */
#define AWORKER_IPC_VERSION		1
#define AWORKER_IPC_HDR_LEN		16
#define AWORKER_IPC_MAX_FRAME		4096
#define AWORKER_IPC_MAX_DATA		2048
#define AWORKER_IPC_ADDR_LEN		72
#define AWORKER_IPC_DIGITS_LEN		32
#define AWORKER_IPC_MAX_IDENT		64
#define AWORKER_IPC_MAX_USER_NAME	32
#define AWORKER_IPC_MAX_CS7		8
#define AWORKER_IPC_CONN_ID_MAX		0x00fffffe
#define AWORKER_IPC_CONN_ID_NONE	0xffffffff

enum aworker_ipc_role {
	AWORKER_IPC_ROLE_MAIN = 1,
	AWORKER_IPC_ROLE_WORKER = 2,
};

enum aworker_ipc_msg_type {
	AWORKER_IPC_MT_HELLO = 0x01,
	AWORKER_IPC_MT_ECHO_REQ = 0x02,
	AWORKER_IPC_MT_ECHO_RSP = 0x03,
	AWORKER_IPC_MT_GOODBYE = 0x04,
	AWORKER_IPC_MT_CS7_INFO = 0x08,
	AWORKER_IPC_MT_USER_BIND = 0x10,
	AWORKER_IPC_MT_USER_UNBIND = 0x11,
	AWORKER_IPC_MT_USER_BIND_RESULT = 0x18,
	AWORKER_IPC_MT_TX_UNITDATA = 0x20,
	AWORKER_IPC_MT_TX_CONN_REQ = 0x21,
	AWORKER_IPC_MT_TX_CONN_RESP = 0x22,
	AWORKER_IPC_MT_TX_DATA = 0x23,
	AWORKER_IPC_MT_TX_DISCONN = 0x24,
	AWORKER_IPC_MT_SCU_UP = 0x30,
	AWORKER_IPC_MT_TX_FAILED = 0x31,
};

enum aworker_ipc_goodbye_reason {
	AWORKER_IPC_GOODBYE_SHUTDOWN = 1,
};

enum aworker_ipc_bind_result {
	AWORKER_IPC_BIND_OK = 0,
	AWORKER_IPC_BIND_NO_SUCH_CS7 = 1,
	AWORKER_IPC_BIND_FAILED = 2,
	AWORKER_IPC_BIND_ALREADY_BOUND = 3,
};

enum aworker_ipc_tx_disposition {
	AWORKER_IPC_TXF_NOT_EXECUTED = 1,
	AWORKER_IPC_TXF_EXECUTED_ERROR = 2,
};

enum aworker_ipc_tx_failed_code {
	AWORKER_IPC_TXF_NO_SUCH_USER = 1,
	AWORKER_IPC_TXF_CONN_ID_IN_USE = 2,
	AWORKER_IPC_TXF_CONN_UNKNOWN = 3,
	AWORKER_IPC_TXF_DATA_TOO_LONG = 4,
	AWORKER_IPC_TXF_LIB_ERROR = 16,
};

enum aworker_ipc_post_conn {
	AWORKER_IPC_POST_CONN_NA = 0,
	AWORKER_IPC_POST_CONN_PRESENT = 1,
	AWORKER_IPC_POST_CONN_ABSENT = 2,
};

/*! Decode errors; aworker_ipc_dec() returns the negative value. */
enum aworker_ipc_err {
	AWORKER_IPC_ERR_NONE = 0,
	AWORKER_IPC_ERR_FRAME_TOO_SHORT,
	AWORKER_IPC_ERR_FRAME_TOO_LONG,
	AWORKER_IPC_ERR_BAD_MAGIC,
	AWORKER_IPC_ERR_BAD_VERSION,
	AWORKER_IPC_ERR_BAD_FLAGS,
	AWORKER_IPC_ERR_BAD_BODY_LEN,
	AWORKER_IPC_ERR_UNKNOWN_TYPE,
	AWORKER_IPC_ERR_WRONG_DIRECTION,
	AWORKER_IPC_ERR_BAD_USER_REF,
	AWORKER_IPC_ERR_TRUNCATED,
	AWORKER_IPC_ERR_TRAILING_DATA,
	AWORKER_IPC_ERR_BAD_RESERVED,
	AWORKER_IPC_ERR_BAD_ADDR,
	AWORKER_IPC_ERR_BAD_CONN_ID,
	AWORKER_IPC_ERR_DATA_TOO_LONG,
	AWORKER_IPC_ERR_UNEXPECTED_DATA,
	AWORKER_IPC_ERR_BAD_PRIM,
	AWORKER_IPC_ERR_BAD_VALUE,
	AWORKER_IPC_ERR_BAD_STRING,
	_AWORKER_IPC_ERR_MAX
};

const char *aworker_ipc_err_name(int err);
const char *aworker_ipc_msg_type_name(uint8_t msg_type);

/*! A view of a DATA blob inside a received record; only valid while that record is. */
struct aworker_ipc_data {
	const uint8_t *buf;
	uint16_t len;
};

struct aworker_ipc_hello {
	uint8_t role;			/*!< enum aworker_ipc_role */
	uint16_t worker_idx;
	uint16_t max_frame;
	uint16_t max_data;
	uint32_t pid;
	char ident[AWORKER_IPC_MAX_IDENT + 1];	/*!< NUL terminated */
};

/*! One cs7 instance a worker owns, as reported in CS7_INFO. */
struct aworker_ipc_cs7_entry {
	uint32_t cs7_id;
	uint32_t primary_pc;
	uint8_t asp_proto;		/*!< enum osmo_ss7_asp_protocol of the instance's Application Server(s) */
	uint8_t num_as;			/*!< number of Application Servers on that cs7 instance */
};

struct aworker_ipc_cs7_info {
	uint8_t count;
	struct aworker_ipc_cs7_entry e[AWORKER_IPC_MAX_CS7];
};

struct aworker_ipc_user_bind {
	uint32_t cs7_id;
	uint16_t ssn;
	char name[AWORKER_IPC_MAX_USER_NAME + 1];	/*!< NUL terminated */
};

struct aworker_ipc_tx_failed {
	uint32_t ref_seq;
	uint8_t ref_type;		/*!< enum aworker_ipc_msg_type, one of the TX_* types */
	uint8_t disposition;		/*!< enum aworker_ipc_tx_disposition */
	uint16_t code;			/*!< enum aworker_ipc_tx_failed_code */
	uint32_t conn_id;
	int32_t lib_rc;
	uint8_t post_conn;		/*!< enum aworker_ipc_post_conn */
};

/*! A decoded message (in-memory representation only, never put on the wire). */
struct aworker_ipc_msg {
	uint8_t type;			/*!< enum aworker_ipc_msg_type */
	uint32_t seq;
	uint32_t user_ref;
	union {
		struct aworker_ipc_hello hello;
		uint32_t echo_token;
		uint8_t goodbye_reason;
		struct aworker_ipc_cs7_info cs7_info;
		struct aworker_ipc_user_bind user_bind;
		uint16_t bind_result;
		struct {
			struct osmo_sccp_addr calling;
			struct osmo_sccp_addr called;
		} tx_unitdata;
		struct {
			uint32_t conn_id;
			bool calling_present;
			struct osmo_sccp_addr calling;	/*!< all zero if !calling_present */
			struct osmo_sccp_addr called;
		} tx_conn_req;
		struct {
			uint32_t conn_id;
			struct osmo_sccp_addr resp;
		} tx_conn_resp;
		struct {
			uint32_t conn_id;
		} tx_data;
		struct {
			uint32_t conn_id;
			bool resp_present;
			struct osmo_sccp_addr resp;	/*!< all zero if !resp_present */
			uint32_t cause;
		} tx_disconn;
		/*! SCU_UP: oph.sap = SCCP_SAP_USER, oph.primitive, oph.operation and the parameters are set;
		 * oph.msg is NULL. Use aworker_ipc_build_scu_prim() to obtain a primitive inside a msgb. */
		struct osmo_scu_prim scu_up;
		struct aworker_ipc_tx_failed tx_failed;
	} u;
	/*! DATA of TX_UNITDATA, TX_CONN_REQ, TX_CONN_RESP, TX_DATA and SCU_UP; len == 0 otherwise. */
	struct aworker_ipc_data data;
};

/* Decoding */

int aworker_ipc_dec(struct aworker_ipc_msg *out, const uint8_t *buf, size_t len, enum aworker_ipc_role receiver);
bool aworker_ipc_type_valid_for(uint8_t msg_type, enum aworker_ipc_role receiver);

struct msgb *aworker_ipc_build_scu_prim(void *ctx, const struct aworker_ipc_msg *msg);

/* Encoding. Every encoder returns 0 and stores a new msgb holding exactly one record in *out, or returns a negative
 * errno and leaves *out NULL. The sequence number is left 0; aworker_ipc_link_send() assigns it.
 * Encoders taking a 'struct msgb *data' ALWAYS take ownership of it (also on error), like the osmo_sccp_tx_*_msg()
 * helpers, and carry msgb_data()/msgb_length() of it. Encoders taking a pointer + length do not take ownership. */

int aworker_ipc_enc_hello(void *ctx, struct msgb **out, enum aworker_ipc_role role, uint16_t worker_idx,
			  uint32_t pid, const char *ident);
int aworker_ipc_enc_echo(void *ctx, struct msgb **out, bool response, uint32_t token);
int aworker_ipc_enc_goodbye(void *ctx, struct msgb **out, uint8_t reason);
int aworker_ipc_enc_cs7_info(void *ctx, struct msgb **out, const struct aworker_ipc_cs7_info *info);
int aworker_ipc_enc_user_bind(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t cs7_id, uint16_t ssn,
			      const char *name);
int aworker_ipc_enc_user_unbind(void *ctx, struct msgb **out, uint32_t user_ref);
int aworker_ipc_enc_user_bind_result(void *ctx, struct msgb **out, uint32_t user_ref, uint16_t result);
int aworker_ipc_enc_tx_unitdata(void *ctx, struct msgb **out, uint32_t user_ref,
				const struct osmo_sccp_addr *calling_addr, const struct osmo_sccp_addr *called_addr,
				struct msgb *data);
int aworker_ipc_enc_tx_conn_req(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
				const struct osmo_sccp_addr *calling_addr, const struct osmo_sccp_addr *called_addr,
				struct msgb *data);
int aworker_ipc_enc_tx_conn_resp(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
				 const struct osmo_sccp_addr *resp_addr, const uint8_t *data, unsigned int len);
int aworker_ipc_enc_tx_data(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id, struct msgb *data);
int aworker_ipc_enc_tx_disconn(void *ctx, struct msgb **out, uint32_t user_ref, uint32_t conn_id,
			       const struct osmo_sccp_addr *resp_addr, uint32_t cause);
int aworker_ipc_enc_scu_up(void *ctx, struct msgb **out, uint32_t user_ref, const struct osmo_scu_prim *prim,
			   const uint8_t *data, unsigned int len);
int aworker_ipc_enc_tx_failed(void *ctx, struct msgb **out, uint32_t user_ref, const struct aworker_ipc_tx_failed *f);

/* Frame header helpers (used by the link) */
uint8_t aworker_ipc_frame_type(const uint8_t *buf);
uint32_t aworker_ipc_frame_seq(const uint8_t *buf);
void aworker_ipc_frame_set_seq(uint8_t *buf, uint32_t seq);

/*! Largest user data of a TX_* request that the pinned libosmo-sigtran sends completely and correctly, without
 * panicking, over M3UA and IPA, for any address the codec accepts: TX_UNITDATA 1139 (bounded by IPA; M3UA alone would
 * allow 1443), TX_CONN_REQ / TX_CONN_RESP / TX_DATA 255, TX_DISCONN 0. No size is safe over SUA, whose transmit path
 * asserts in this libosmo-sigtran version. See aworker_ipc.c for the derivation. Returns -EINVAL for other types. */
int aworker_ipc_sccp_max_data(uint8_t tx_msg_type);
/*! 0 if len is within aworker_ipc_sccp_max_data(tx_msg_type), -EMSGSIZE if not, -EINVAL for a non-TX type. */
int aworker_ipc_sccp_check_data(uint8_t tx_msg_type, unsigned int len);

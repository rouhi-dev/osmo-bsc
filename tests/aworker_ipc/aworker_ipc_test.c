/* Tests for the A-worker IPC codec (aworker_ipc.c) and link (aworker_ipc_link.c) */
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

/* The link tests run the real libosmocore osmo_io implementation on real socketpair()s. Kernel behaviour that an
 * AF_UNIX SOCK_SEQPACKET socket cannot produce (partial sends, spurious EAGAIN, ...) is injected by defining
 * sendmsg() in this executable: it pre-empts the C library's sendmsg() for the calls made by libosmocore, and
 * otherwise forwards to the real one. This only works for the poll backend (io_uring does not call sendmsg()). */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/gsm/ipa.h>
#include <osmocom/sccp/sccp_types.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/sigtran_sap.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_ipc_link.h>

static void *ctx;
static int failures;

#define CHECK(cond) do { \
		if (!(cond)) { \
			printf("  FAILED: %s (line %d)\n", #cond, __LINE__); \
			failures++; \
		} \
	} while (0)

static const char *errno_name(int rc)
{
	switch (rc) {
	case 0: return "0";
	case -EINVAL: return "-EINVAL";
	case -EMSGSIZE: return "-EMSGSIZE";
	case -ENOTSUP: return "-ENOTSUP";
	case -ENOMEM: return "-ENOMEM";
	case -EBUSY: return "-EBUSY";
	case -ENOTCONN: return "-ENOTCONN";
	case -ENOSPC: return "-ENOSPC";
	case -EPIPE: return "-EPIPE";
	case -EPROTOTYPE: return "-EPROTOTYPE";
	case -EAFNOSUPPORT: return "-EAFNOSUPPORT";
	case -ENOTSOCK: return "-ENOTSOCK";
	default: return "(other)";
	}
}

/***********************************************************************
 * sendmsg() fault injection
 ***********************************************************************/

enum inj_action {
	INJ_NONE,
	INJ_SHORT,	/* send nothing, report len - 1 */
	INJ_ZERO,	/* send nothing, report 0 */
	INJ_EAGAIN,	/* send nothing, fail with EAGAIN */
	INJ_EMSGSIZE,	/* send nothing, fail with EMSGSIZE */
};

static struct {
	int fd;
	enum inj_action action;	/* applied once, to the next sendmsg() on fd */
	unsigned int calls;	/* sendmsg() calls on fd */
	unsigned int injected;
} inj = { .fd = -1 };

static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
	if (!real_sendmsg)
		real_sendmsg = (ssize_t (*)(int, const struct msghdr *, int))dlsym(RTLD_NEXT, "sendmsg");
	if (fd == inj.fd) {
		enum inj_action a = inj.action;
		size_t len = 0, i;

		inj.calls++;
		inj.action = INJ_NONE;
		for (i = 0; i < msg->msg_iovlen; i++)
			len += msg->msg_iov[i].iov_len;
		switch (a) {
		case INJ_SHORT:
			inj.injected++;
			return len - 1;
		case INJ_ZERO:
			inj.injected++;
			return 0;
		case INJ_EAGAIN:
			inj.injected++;
			errno = EAGAIN;
			return -1;
		case INJ_EMSGSIZE:
			inj.injected++;
			errno = EMSGSIZE;
			return -1;
		case INJ_NONE:
			break;
		}
	}
	return real_sendmsg(fd, msg, flags);
}

/***********************************************************************
 * Helpers
 ***********************************************************************/

static void print_hex(const char *label, const uint8_t *buf, size_t len)
{
	size_t i;
	printf("  %s (%zu):", label, len);
	for (i = 0; i < len; i++) {
		if (i % 32 == 0)
			printf("\n   ");
		printf(" %02x", buf[i]);
	}
	printf("\n");
}

static struct msgb *data_msgb(unsigned int len, uint8_t seed)
{
	struct msgb *msg = msgb_alloc_c(ctx, 4096, "test data");
	unsigned int i;
	for (i = 0; i < len; i++)
		msgb_put_u8(msg, (uint8_t)(seed + i));
	return msg;
}

static bool data_ok(const struct aworker_ipc_data *d, unsigned int len, uint8_t seed)
{
	unsigned int i;
	if (d->len != len)
		return false;
	for (i = 0; i < len; i++) {
		if (d->buf[i] != (uint8_t)(seed + i))
			return false;
	}
	return true;
}

/* Addresses; memset first so that struct padding is zero and memcmp() can be used */
static void addr_gt(struct osmo_sccp_addr *a)
{
	memset(a, 0, sizeof(*a));
	a->presence = OSMO_SCCP_ADDR_T_GT | OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN;
	a->ri = OSMO_SCCP_RI_GT;
	a->gt.gti = OSMO_SCCP_GTI_TT_NPL_ENC_NAI;
	a->gt.tt = 7;
	a->gt.npi = OSMO_SCCP_NPI_E164_ISDN;
	a->gt.nai = OSMO_SCCP_NAI_INTL;
	OSMO_STRLCPY_ARRAY(a->gt.digits, "4930123456789012345678901234567");	/* 31 digits */
	a->pc = 0x123456;
	a->ssn = OSMO_SCCP_SSN_BSSAP;
}

static void addr_ip6(struct osmo_sccp_addr *a)
{
	memset(a, 0, sizeof(*a));
	a->presence = OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN | OSMO_SCCP_ADDR_T_IPv6;
	a->ri = OSMO_SCCP_RI_SSN_IP;
	a->pc = 7;
	a->ssn = 8;
	inet_pton(AF_INET6, "2001:db8::1", &a->ip.v6);
}

static void addr_ip4(struct osmo_sccp_addr *a)
{
	memset(a, 0, sizeof(*a));
	a->presence = OSMO_SCCP_ADDR_T_SSN | OSMO_SCCP_ADDR_T_IPv4;
	a->ri = OSMO_SCCP_RI_SSN_IP;
	a->ssn = OSMO_SCCP_SSN_RANAP;
	inet_pton(AF_INET, "192.0.2.1", &a->ip.v4);
}

static void addr_pc(struct osmo_sccp_addr *a, uint32_t pc)
{
	memset(a, 0, sizeof(*a));
	a->presence = OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN;
	a->ri = OSMO_SCCP_RI_SSN_PC;
	a->pc = pc;
	a->ssn = OSMO_SCCP_SSN_BSSAP;
}

static bool addr_eq(const struct osmo_sccp_addr *a, const struct osmo_sccp_addr *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

static int dec(struct aworker_ipc_msg *m, const struct msgb *msg, enum aworker_ipc_role receiver)
{
	return aworker_ipc_dec(m, msgb_data(msg), msgb_length(msg), receiver);
}

static void scu_prim_init(struct osmo_scu_prim *p, unsigned int primitive, enum osmo_prim_operation op)
{
	memset(p, 0, sizeof(*p));
	osmo_prim_init(&p->oph, SCCP_SAP_USER, primitive, op, NULL);
}

/***********************************************************************
 * C1: main -> worker round trips
 ***********************************************************************/

static void test_c1_downlink_roundtrip(void)
{
	static const unsigned int lens[] = { 0, 1, AWORKER_IPC_MAX_DATA };
	struct osmo_sccp_addr a_gt, a_ip6, a_ip4;
	struct aworker_ipc_msg m;
	struct msgb *msg;
	unsigned int i;
	int rc;

	printf("C1: main->worker round trips\n");
	addr_gt(&a_gt);
	addr_ip6(&a_ip6);
	addr_ip4(&a_ip4);

	for (i = 0; i < ARRAY_SIZE(lens); i++) {
		unsigned int len = lens[i];

		rc = aworker_ipc_enc_tx_unitdata(ctx, &msg, 11, &a_gt, &a_ip6, data_msgb(len, 1));
		CHECK(rc == 0);
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
		CHECK(m.type == AWORKER_IPC_MT_TX_UNITDATA && m.user_ref == 11);
		CHECK(addr_eq(&m.u.tx_unitdata.calling, &a_gt) && addr_eq(&m.u.tx_unitdata.called, &a_ip6));
		CHECK(data_ok(&m.data, len, 1));
		printf("  TX_UNITDATA len=%u: %u octets\n", len, msgb_length(msg));
		msgb_free(msg);

		rc = aworker_ipc_enc_tx_conn_req(ctx, &msg, 12, 0x00fffffe, &a_ip4, &a_gt, data_msgb(len, 2));
		CHECK(rc == 0);
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
		CHECK(m.u.tx_conn_req.conn_id == 0x00fffffe && m.u.tx_conn_req.calling_present);
		CHECK(addr_eq(&m.u.tx_conn_req.calling, &a_ip4) && addr_eq(&m.u.tx_conn_req.called, &a_gt));
		CHECK(data_ok(&m.data, len, 2));
		printf("  TX_CONN_REQ calling=present len=%u: %u octets\n", len, msgb_length(msg));
		msgb_free(msg);

		rc = aworker_ipc_enc_tx_conn_req(ctx, &msg, 12, 0, NULL, &a_gt, data_msgb(len, 3));
		CHECK(rc == 0);
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
		{
			struct osmo_sccp_addr zero;
			memset(&zero, 0, sizeof(zero));
			CHECK(m.u.tx_conn_req.conn_id == 0 && !m.u.tx_conn_req.calling_present);
			CHECK(addr_eq(&m.u.tx_conn_req.calling, &zero));
		}
		CHECK(data_ok(&m.data, len, 3));
		printf("  TX_CONN_REQ calling=absent len=%u: %u octets\n", len, msgb_length(msg));
		msgb_free(msg);

		{
			struct msgb *d = data_msgb(len, 4);
			rc = aworker_ipc_enc_tx_conn_resp(ctx, &msg, 13, 42, &a_ip6, msgb_data(d), msgb_length(d));
			msgb_free(d);
		}
		CHECK(rc == 0);
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
		CHECK(m.u.tx_conn_resp.conn_id == 42 && addr_eq(&m.u.tx_conn_resp.resp, &a_ip6));
		CHECK(data_ok(&m.data, len, 4));
		printf("  TX_CONN_RESP len=%u: %u octets\n", len, msgb_length(msg));
		msgb_free(msg);

		rc = aworker_ipc_enc_tx_data(ctx, &msg, 14, 43, data_msgb(len, 5));
		CHECK(rc == 0);
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
		CHECK(m.u.tx_data.conn_id == 43 && data_ok(&m.data, len, 5));
		printf("  TX_DATA len=%u: %u octets\n", len, msgb_length(msg));
		msgb_free(msg);
	}

	rc = aworker_ipc_enc_tx_disconn(ctx, &msg, 15, 44, &a_ip4, 0xdeadbeef);
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
	CHECK(m.u.tx_disconn.conn_id == 44 && m.u.tx_disconn.resp_present && m.u.tx_disconn.cause == 0xdeadbeef);
	CHECK(addr_eq(&m.u.tx_disconn.resp, &a_ip4) && m.data.len == 0);
	printf("  TX_DISCONN resp=present: %u octets\n", msgb_length(msg));
	msgb_free(msg);

	rc = aworker_ipc_enc_tx_disconn(ctx, &msg, 15, 45, NULL, 0);
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
	CHECK(m.u.tx_disconn.conn_id == 45 && !m.u.tx_disconn.resp_present);
	printf("  TX_DISCONN resp=absent: %u octets\n", msgb_length(msg));
	msgb_free(msg);

	rc = aworker_ipc_enc_user_bind(ctx, &msg, 16, 1, OSMO_SCCP_SSN_BSSAP, "msc-0");
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
	CHECK(m.u.user_bind.cs7_id == 1 && m.u.user_bind.ssn == 254 && !strcmp(m.u.user_bind.name, "msc-0"));
	printf("  USER_BIND: %u octets\n", msgb_length(msg));
	msgb_free(msg);

	rc = aworker_ipc_enc_user_unbind(ctx, &msg, 16);
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0);
	CHECK(m.type == AWORKER_IPC_MT_USER_UNBIND && m.user_ref == 16);
	printf("  USER_UNBIND: %u octets\n", msgb_length(msg));
	msgb_free(msg);

	rc = aworker_ipc_enc_echo(ctx, &msg, false, 0x01020304);
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0 && m.u.echo_token == 0x01020304);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_MAIN) == 0);
	msgb_free(msg);

	rc = aworker_ipc_enc_goodbye(ctx, &msg, AWORKER_IPC_GOODBYE_SHUTDOWN);
	CHECK(rc == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_MAIN) == 0 && m.u.goodbye_reason == AWORKER_IPC_GOODBYE_SHUTDOWN);
	msgb_free(msg);
	printf("  ECHO_REQ, GOODBYE: decoded by both roles\n");
}

/***********************************************************************
 * C2: worker -> main round trips, including the rebuilt primitive msgb
 ***********************************************************************/

struct up_case {
	const char *name;
	unsigned int primitive;
	enum osmo_prim_operation op;
	unsigned int len;
	bool co;	/* connection-oriented allocation (1024) or connectionless (2048 + 512 headroom) */
};

static void fill_up_params(struct osmo_scu_prim *p)
{
	switch (p->oph.primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
		addr_gt(&p->u.connect.called_addr);
		addr_ip6(&p->u.connect.calling_addr);
		addr_ip4(&p->u.connect.responding_addr);
		p->u.connect.sccp_class = 2;
		p->u.connect.importance = 0x11223344;
		p->u.connect.conn_id = 0x00abcdef;
		break;
	case OSMO_SCU_PRIM_N_DATA:
		p->u.data.conn_id = 0x00fffffe;
		p->u.data.importance = 0xffffffff;
		break;
	case OSMO_SCU_PRIM_N_DISCONNECT:
		p->u.disconnect.originator = OSMO_SCCP_ORIG_NS_PROVIDER;
		addr_pc(&p->u.disconnect.responding_addr, 0x00ffffff);
		p->u.disconnect.cause = 0x55;
		p->u.disconnect.conn_id = 1;
		p->u.disconnect.importance = 3;
		break;
	case OSMO_SCU_PRIM_N_UNITDATA:
		addr_pc(&p->u.unitdata.called_addr, 1);
		addr_gt(&p->u.unitdata.calling_addr);
		p->u.unitdata.in_sequence_control = OSMO_SCU_UNITDATA_REQ_P_SEQUENCE_CONTROL_NOT_PRESENT;
		p->u.unitdata.return_option = 1;
		p->u.unitdata.importance = 2;
		break;
	case OSMO_SCU_PRIM_N_NOTICE:
		addr_ip4(&p->u.notice.called_addr);
		addr_pc(&p->u.notice.calling_addr, 2);
		p->u.notice.cause = 0x0b;
		p->u.notice.importance = 4;
		break;
	case OSMO_SCU_PRIM_N_STATE:
		p->u.state.affected_pc = 0x1234;
		p->u.state.affected_ssn = 254;
		p->u.state.user_in_service = true;
		p->u.state.ssn_multiplicity_ind = 9;
		break;
	case OSMO_SCU_PRIM_N_PCSTATE:
		p->u.pcstate.affected_pc = 0x4321;
		p->u.pcstate.restricted_importance_level = 6;
		p->u.pcstate.sp_status = OSMO_SCCP_SP_S_ACCESSIBLE;
		p->u.pcstate.remote_sccp_status = OSMO_SCCP_REM_SCCP_S_CONGESTED;
		break;
	}
}

static void test_c2_uplink_roundtrip(void)
{
	static const struct up_case cases[] = {
		{ "N-CONNECT.ind", OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION, 100, true },
		{ "N-CONNECT.cnf", OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_CONFIRM, 0, true },
		{ "N-DATA.ind", OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION, 255, true },
		{ "N-DISCONNECT.ind", OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION, 3, true },
		{ "N-UNITDATA.ind", OSMO_SCU_PRIM_N_UNITDATA, PRIM_OP_INDICATION, AWORKER_IPC_MAX_DATA, false },
		{ "N-NOTICE.ind", OSMO_SCU_PRIM_N_NOTICE, PRIM_OP_INDICATION, 7, false },
		{ "N-STATE.ind", OSMO_SCU_PRIM_N_STATE, PRIM_OP_INDICATION, 0, false },
		{ "N-PCSTATE.ind", OSMO_SCU_PRIM_N_PCSTATE, PRIM_OP_INDICATION, 0, false },
	};
	unsigned int i;

	printf("C2: worker->main round trips and rebuilt primitives\n");
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct up_case *c = &cases[i];
		struct osmo_scu_prim p, *q;
		struct aworker_ipc_msg m;
		struct msgb *msg, *d, *up;
		int rc;

		scu_prim_init(&p, c->primitive, c->op);
		fill_up_params(&p);
		d = data_msgb(c->len, 9);
		rc = aworker_ipc_enc_scu_up(ctx, &msg, 21, &p, msgb_data(d), msgb_length(d));
		msgb_free(d);
		CHECK(rc == 0);
		if (rc)
			continue;
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_MAIN) == 0);
		CHECK(m.type == AWORKER_IPC_MT_SCU_UP && m.user_ref == 21);
		CHECK(memcmp(&m.u.scu_up, &p, sizeof(p)) == 0);
		CHECK(data_ok(&m.data, c->len, 9));

		up = aworker_ipc_build_scu_prim(ctx, &m);
		CHECK(up != NULL);
		if (up) {
			q = (struct osmo_scu_prim *)msgb_data(up);
			CHECK(q->oph.msg == up);
			CHECK(q->oph.sap == SCCP_SAP_USER && q->oph.primitive == c->primitive
			      && q->oph.operation == c->op);
			CHECK(memcmp(&q->u, &p.u, sizeof(p.u)) == 0);
			CHECK(msgb_l2(up) == msgb_data(up) + sizeof(*q));
			CHECK(msgb_l2len(up) == c->len);
			CHECK(c->len == 0 || ((uint8_t *)msgb_l2(up))[0] == 9);
			if (c->co)
				CHECK(up->data_len == 1024 && msgb_headroom(up) == 0);
			else
				CHECK(up->data_len == 2048 + 512 + (c->len + sizeof(*q) > 2048 ? c->len + sizeof(*q) - 2048 : 0)
				      && msgb_headroom(up) == 512);
			msgb_free(up);
		}
		printf("  %s len=%u: %u octets, primitive rebuilt (%s allocation)\n", c->name, c->len,
		       msgb_length(msg), c->co ? "connection-oriented" : "connectionless");
		msgb_free(msg);
	}
}

/***********************************************************************
 * C3: golden vectors (byte-exact wire format)
 ***********************************************************************/

static void test_c3_golden(void)
{
	struct osmo_sccp_addr a;
	struct osmo_scu_prim p;
	struct aworker_ipc_tx_failed f = {
		.ref_seq = 0x01020304, .ref_type = AWORKER_IPC_MT_TX_CONN_REQ,
		.disposition = AWORKER_IPC_TXF_EXECUTED_ERROR, .code = AWORKER_IPC_TXF_LIB_ERROR,
		.conn_id = 0x00abcdef, .lib_rc = -22, .post_conn = AWORKER_IPC_POST_CONN_ABSENT,
	};
	struct msgb *msg;

	printf("C3: golden vectors\n");
	CHECK(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_MAIN, 3, 0x01020304, "osmo-bsc") == 0);
	aworker_ipc_frame_set_seq(msgb_data(msg), 1);
	print_hex("HELLO main idx=3", msgb_data(msg), msgb_length(msg));
	msgb_free(msg);

	CHECK(aworker_ipc_enc_tx_data(ctx, &msg, 0x0a0b0c0d, 0x00abcdef, data_msgb(3, 1)) == 0);
	aworker_ipc_frame_set_seq(msgb_data(msg), 2);
	print_hex("TX_DATA", msgb_data(msg), msgb_length(msg));
	msgb_free(msg);

	addr_gt(&a);
	CHECK(aworker_ipc_enc_tx_disconn(ctx, &msg, 5, 0x000102, &a, 0x80) == 0);
	aworker_ipc_frame_set_seq(msgb_data(msg), 0xfffffffe);
	print_hex("TX_DISCONN", msgb_data(msg), msgb_length(msg));
	msgb_free(msg);

	scu_prim_init(&p, OSMO_SCU_PRIM_N_PCSTATE, PRIM_OP_INDICATION);
	fill_up_params(&p);
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 7, &p, NULL, 0) == 0);
	print_hex("SCU_UP N-PCSTATE.ind", msgb_data(msg), msgb_length(msg));
	msgb_free(msg);

	CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 9, &f) == 0);
	print_hex("TX_FAILED", msgb_data(msg), msgb_length(msg));
	msgb_free(msg);
}

/***********************************************************************
 * C4: boundaries, C7: ownership of consumed msgbs on error paths
 ***********************************************************************/

static void test_c4_boundaries(void)
{
	struct osmo_sccp_addr a, bad;
	struct aworker_ipc_msg m;
	struct msgb *msg = NULL;
	size_t blocks = talloc_total_blocks(ctx);
	int rc;

	printf("C4: boundaries and C7: msgb ownership on error paths\n");
	addr_pc(&a, 1);

	rc = aworker_ipc_enc_tx_data(ctx, &msg, 1, 1, data_msgb(AWORKER_IPC_MAX_DATA + 1, 0));
	printf("  TX_DATA len=%u: %s\n", AWORKER_IPC_MAX_DATA + 1, errno_name(rc));
	CHECK(rc == -EMSGSIZE && msg == NULL);
	rc = aworker_ipc_enc_tx_unitdata(ctx, &msg, 1, &a, &a, data_msgb(AWORKER_IPC_MAX_DATA + 1, 0));
	printf("  TX_UNITDATA len=%u: %s\n", AWORKER_IPC_MAX_DATA + 1, errno_name(rc));
	CHECK(rc == -EMSGSIZE);
	rc = aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, 1, NULL, &a, data_msgb(AWORKER_IPC_MAX_DATA + 1, 0));
	printf("  TX_CONN_REQ len=%u: %s\n", AWORKER_IPC_MAX_DATA + 1, errno_name(rc));
	CHECK(rc == -EMSGSIZE);
	{
		uint8_t big[AWORKER_IPC_MAX_DATA + 1] = {};
		rc = aworker_ipc_enc_tx_conn_resp(ctx, &msg, 1, 1, &a, big, sizeof(big));
		printf("  TX_CONN_RESP len=%zu: %s\n", sizeof(big), errno_name(rc));
		CHECK(rc == -EMSGSIZE);
	}

	rc = aworker_ipc_enc_tx_data(ctx, &msg, 1, AWORKER_IPC_CONN_ID_MAX + 1, data_msgb(1, 0));
	printf("  TX_DATA conn_id=0x%x: %s\n", AWORKER_IPC_CONN_ID_MAX + 1, errno_name(rc));
	CHECK(rc == -EINVAL);
	rc = aworker_ipc_enc_tx_data(ctx, &msg, 0, 1, data_msgb(1, 0));
	printf("  TX_DATA user_ref=0: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);
	rc = aworker_ipc_enc_tx_conn_resp(ctx, &msg, 1, 1, NULL, NULL, 0);
	printf("  TX_CONN_RESP without responding address: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);

	bad = a;
	OSMO_STRLCPY_ARRAY(bad.gt.digits, "");
	memset(bad.gt.digits, '1', sizeof(bad.gt.digits));	/* 32 digits: no room for the NUL */
	rc = aworker_ipc_enc_tx_unitdata(ctx, &msg, 1, &bad, &a, data_msgb(1, 0));
	printf("  address with 32 GT digits: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);
	bad = a;
	bad.ssn = 256;
	rc = aworker_ipc_enc_tx_unitdata(ctx, &msg, 1, &a, &bad, data_msgb(1, 0));
	printf("  address with ssn 256: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);
	bad = a;
	bad.pc = 0x01000000;
	rc = aworker_ipc_enc_tx_disconn(ctx, &msg, 1, 1, &bad, 0);
	printf("  address with pc 0x1000000: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);
	bad = a;
	bad.presence |= 0x20;
	rc = aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, 1, &bad, &a, data_msgb(1, 0));
	printf("  address with unknown presence bit: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);

	{
		struct osmo_scu_prim p;
		scu_prim_init(&p, OSMO_SCU_PRIM_N_DATA, PRIM_OP_REQUEST);
		rc = aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, NULL, 0);
		printf("  SCU_UP N-DATA.req: %s\n", errno_name(rc));
		CHECK(rc == -ENOTSUP);
		scu_prim_init(&p, OSMO_SCU_PRIM_N_RESET, PRIM_OP_INDICATION);
		rc = aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, NULL, 0);
		printf("  SCU_UP N-RESET.ind: %s\n", errno_name(rc));
		CHECK(rc == -ENOTSUP);
		scu_prim_init(&p, OSMO_SCU_PRIM_N_PCSTATE, PRIM_OP_INDICATION);
		rc = aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, (const uint8_t *)"x", 1);
		printf("  SCU_UP N-PCSTATE.ind with data: %s\n", errno_name(rc));
		CHECK(rc == -EINVAL);
	}

	CHECK(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_MAIN, 0, 0, "bad\tident") == -EINVAL);
	CHECK(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 256, "x") == -EINVAL);
	CHECK(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 1, "") == -EINVAL);
	CHECK(aworker_ipc_enc_goodbye(ctx, &msg, 2) == -EINVAL);
	CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, 1, 4) == -EINVAL);
	printf("  invalid HELLO ident, USER_BIND ssn/name, GOODBYE reason, BIND_RESULT: -EINVAL\n");

	/* exact limits are accepted */
	CHECK(aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, AWORKER_IPC_CONN_ID_MAX, NULL, &a,
					  data_msgb(AWORKER_IPC_MAX_DATA, 0)) == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == 0 && m.data.len == AWORKER_IPC_MAX_DATA);
	msgb_free(msg);
	printf("  conn_id 0x%x with %u octets of data: accepted\n", AWORKER_IPC_CONN_ID_MAX, AWORKER_IPC_MAX_DATA);

	printf("  talloc blocks after error paths: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAK");
	CHECK(talloc_total_blocks(ctx) == blocks);
}

/***********************************************************************
 * C5 / C8 / C9: malformed records
 ***********************************************************************/

/* Copy a record into a buffer, apply a modification, decode */
struct rec {
	uint8_t b[AWORKER_IPC_MAX_FRAME + 16];
	size_t len;
};

static void rec_from(struct rec *r, struct msgb *msg)
{
	r->len = msgb_length(msg);
	memcpy(r->b, msgb_data(msg), r->len);
	msgb_free(msg);
}

static void rec_fix_body_len(struct rec *r)
{
	r->b[6] = (r->len - AWORKER_IPC_HDR_LEN) >> 8;
	r->b[7] = (r->len - AWORKER_IPC_HDR_LEN) & 0xff;
}

static void expect(const char *what, const struct rec *r, enum aworker_ipc_role receiver, int exp_err)
{
	struct aworker_ipc_msg m;
	int rc = aworker_ipc_dec(&m, r->b, r->len, receiver);
	printf("  %s: %s\n", what, aworker_ipc_err_name(rc));
	CHECK(rc == -exp_err);
}

/* Offsets inside records */
#define OFF_BODY	AWORKER_IPC_HDR_LEN
#define A_PRES		0
#define A_RI		4
#define A_GTI		5
#define A_DLEN		15
#define A_DIGITS	16
#define A_PC		48
#define A_SSN		52
#define A_IP		56

static void test_c5_malformed(void)
{
	struct osmo_sccp_addr a_gt, a_pc, a_ip4;
	struct osmo_scu_prim p;
	struct rec base_cr, base_cr_nocalling, base_hello, base_pcstate, base_state, base_disc, base_txf, base_bind;
	struct rec r;
	struct msgb *msg;
	unsigned int called;	/* offset of the called ADDR in TX_CONN_REQ */
	size_t cut;
	int bad_cut = 0;

	printf("C5/C8/C9: malformed records\n");
	addr_gt(&a_gt);
	addr_pc(&a_pc, 1);
	addr_ip4(&a_ip4);

	CHECK(aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, 5, &a_pc, &a_gt, data_msgb(4, 0)) == 0);
	rec_from(&base_cr, msg);
	CHECK(aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, 5, NULL, &a_gt, data_msgb(4, 0)) == 0);
	rec_from(&base_cr_nocalling, msg);
	CHECK(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_WORKER, 1, 1, "id") == 0);
	rec_from(&base_hello, msg);
	scu_prim_init(&p, OSMO_SCU_PRIM_N_PCSTATE, PRIM_OP_INDICATION);
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, NULL, 0) == 0);
	rec_from(&base_pcstate, msg);
	scu_prim_init(&p, OSMO_SCU_PRIM_N_STATE, PRIM_OP_INDICATION);
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, NULL, 0) == 0);
	rec_from(&base_state, msg);
	scu_prim_init(&p, OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION);
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, NULL, 0) == 0);
	rec_from(&base_disc, msg);
	{
		struct aworker_ipc_tx_failed f = { .ref_type = AWORKER_IPC_MT_TX_DATA, .conn_id = 1,
						   .disposition = AWORKER_IPC_TXF_NOT_EXECUTED,
						   .code = AWORKER_IPC_TXF_CONN_UNKNOWN };
		CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 1, &f) == 0);
		rec_from(&base_txf, msg);
	}
	CHECK(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, 254, "a") == 0);
	rec_from(&base_bind, msg);

	/* every truncation of a valid record is rejected */
	for (cut = 0; cut < base_cr.len; cut++) {
		struct aworker_ipc_msg m;
		if (aworker_ipc_dec(&m, base_cr.b, cut, AWORKER_IPC_ROLE_WORKER) == 0)
			bad_cut++;
	}
	printf("  all %zu truncations of TX_CONN_REQ rejected: %s\n", base_cr.len, bad_cut ? "NO" : "yes");
	CHECK(bad_cut == 0);

	/* header */
	r = base_cr; r.len = AWORKER_IPC_HDR_LEN - 1;
	expect("15 octet record", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_FRAME_TOO_SHORT);
	r = base_cr; r.len = AWORKER_IPC_MAX_FRAME + 1;
	expect("4097 octet record", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_FRAME_TOO_LONG);
	r = base_cr; r.b[1] = 'X';
	expect("bad magic", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_MAGIC);
	r = base_cr; r.b[2] = 0;
	expect("version 0", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_VERSION);
	r = base_cr; r.b[2] = 2;
	expect("version 2", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_VERSION);
	r = base_cr; r.b[5] = 1;
	expect("flags 1", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_FLAGS);
	r = base_cr; r.b[7]++;
	expect("body_len + 1", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_BODY_LEN);
	r = base_cr; r.b[3] = 0x7f;
	expect("unknown type 0x7f", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_UNKNOWN_TYPE);
	r = base_cr;
	expect("TX_CONN_REQ received by main", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_WRONG_DIRECTION);
	r = base_pcstate;
	expect("SCU_UP received by worker", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_WRONG_DIRECTION);
	r = base_cr; memset(r.b + 12, 0, 4);
	expect("TX_CONN_REQ with user_ref 0", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_USER_REF);
	r = base_hello; r.b[15] = 1;
	expect("HELLO with user_ref 1", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_USER_REF);
	r = base_cr; r.b[r.len++] = 0; rec_fix_body_len(&r);
	expect("trailing octet", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_TRAILING_DATA);
	r = base_bind; r.len = OFF_BODY + 3; rec_fix_body_len(&r);
	expect("USER_BIND body cut at 3 (consistent body_len)", &r, AWORKER_IPC_ROLE_WORKER,
	       AWORKER_IPC_ERR_TRUNCATED);

	/* TX_CONN_REQ fields: conn_id u32 | flags u8 | calling ADDR | called ADDR | DATA */
	called = OFF_BODY + 5 + AWORKER_IPC_ADDR_LEN;
	r = base_cr; r.b[OFF_BODY + 1] = 0xff; r.b[OFF_BODY + 2] = 0xff; r.b[OFF_BODY + 3] = 0xff;
	expect("conn_id 0xffffff", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_CONN_ID);
	r = base_cr; r.b[OFF_BODY + 4] = 0x03;
	expect("address flags 0x03", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_RESERVED);
	r = base_cr; r.b[OFF_BODY + 4] = 0x00;
	expect("calling absent but address octets non-zero", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr_nocalling; r.b[OFF_BODY + 5 + A_SSN + 3] = 1;
	expect("calling absent, one non-zero octet", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_PRES + 3] |= 0x20;
	expect("ADDR unknown presence bit", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_RI] = 4;
	expect("ADDR ri 4", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_GTI] = 5;
	expect("ADDR gti 5", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_DLEN] = 32;
	expect("ADDR digits_len 32", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_DIGITS + 5] = 0;
	expect("ADDR NUL inside digits", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_DLEN] = 30;
	expect("ADDR digit after digits_len", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_PC] = 1;
	expect("ADDR pc > 0xffffff", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_SSN + 2] = 1;
	expect("ADDR ssn 256 + 254", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	r = base_cr; r.b[called + A_IP + 15] = 1;
	expect("ADDR ip octet without IP presence", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	{
		struct osmo_sccp_addr a;
		addr_ip4(&a);
		CHECK(aworker_ipc_enc_tx_conn_req(ctx, &msg, 1, 5, NULL, &a, data_msgb(0, 0)) == 0);
		rec_from(&r, msg);
		r.b[called + A_IP + 4] = 1;
		expect("ADDR IPv4 with non-zero octet 5", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_ADDR);
	}
	r = base_cr; r.b[called + AWORKER_IPC_ADDR_LEN] = 0x08; r.b[called + AWORKER_IPC_ADDR_LEN + 1] = 0x01;
	expect("DATA len 2049", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_DATA_TOO_LONG);
	r = base_cr; r.b[called + AWORKER_IPC_ADDR_LEN + 1]++;
	expect("DATA len beyond record", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_TRUNCATED);

	/* SCU_UP: primitive u8 | operation u8 | reserved u16 | ... */
	r = base_pcstate; r.b[OFF_BODY] = OSMO_SCU_PRIM_N_DATA; r.b[OFF_BODY + 1] = PRIM_OP_REQUEST;
	expect("SCU_UP N-DATA.req", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_PRIM);
	r = base_pcstate; r.b[OFF_BODY] = OSMO_SCU_PRIM_N_RESET; r.b[OFF_BODY + 1] = PRIM_OP_INDICATION;
	expect("SCU_UP N-RESET.ind", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_PRIM);
	r = base_pcstate; r.b[OFF_BODY + 3] = 1;
	expect("SCU_UP reserved 1", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_RESERVED);
	r = base_pcstate; r.b[r.len - 1] = 1; r.b[r.len++] = 0xaa; rec_fix_body_len(&r);
	expect("SCU_UP N-PCSTATE.ind with data", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_UNEXPECTED_DATA);
	r = base_state; r.b[OFF_BODY + 4 + 8] = 2;
	expect("SCU_UP N-STATE.ind user_in_service 2", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_disc; r.b[OFF_BODY + 4 + 3] = 3;
	expect("SCU_UP N-DISCONNECT.ind originator 3", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);

	/* C8: HELLO: role u8 | reserved u8 | worker_idx u16 | max_frame u16 | max_data u16 | pid u32 | ident */
	r = base_hello; r.b[OFF_BODY] = 3;
	expect("HELLO role 3", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_hello; r.b[OFF_BODY + 1] = 1;
	expect("HELLO reserved 1", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_RESERVED);
	r = base_hello; r.b[OFF_BODY + 13] = 0x01;
	expect("HELLO ident with control character", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_STRING);
	r = base_hello; r.b[OFF_BODY + 12] = 65;
	expect("HELLO ident_len 65", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_STRING);
	r = base_hello; r.b[OFF_BODY + 12] = 3;
	expect("HELLO ident_len beyond record", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_TRUNCATED);

	/* USER_BIND: cs7_id u32 | ssn u16 | name */
	r = base_bind; r.b[OFF_BODY + 4] = 1;
	expect("USER_BIND ssn 510", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_bind; r.b[OFF_BODY + 6] = 0; r.len--; rec_fix_body_len(&r);
	expect("USER_BIND empty name", &r, AWORKER_IPC_ROLE_WORKER, AWORKER_IPC_ERR_BAD_STRING);

	/* C9: TX_FAILED: ref_seq u32 | ref_type u8 | disposition u8 | code u16 | conn_id u32 | lib_rc i32 |
	 * post_conn u8 | reserved u8[3] */
	r = base_txf;
	expect("TX_FAILED NOT_EXECUTED/CONN_UNKNOWN for TX_DATA (valid)", &r, AWORKER_IPC_ROLE_MAIN,
	       AWORKER_IPC_ERR_NONE);
	r = base_txf; r.b[OFF_BODY + 4] = AWORKER_IPC_MT_USER_BIND;
	expect("TX_FAILED ref_type USER_BIND", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 5] = 3;
	expect("TX_FAILED disposition 3", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 7] = AWORKER_IPC_TXF_CONN_ID_IN_USE;
	expect("TX_FAILED CONN_ID_IN_USE for TX_DATA", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 15] = 0xff;
	expect("TX_FAILED NOT_EXECUTED with lib_rc != 0", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 16] = AWORKER_IPC_POST_CONN_PRESENT;
	expect("TX_FAILED NOT_EXECUTED with post_conn", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 5] = AWORKER_IPC_TXF_EXECUTED_ERROR; r.b[OFF_BODY + 7] = AWORKER_IPC_TXF_LIB_ERROR;
	expect("TX_FAILED EXECUTED_ERROR with lib_rc 0", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	r = base_txf; r.b[OFF_BODY + 4] = AWORKER_IPC_MT_TX_UNITDATA; r.b[OFF_BODY + 7] = AWORKER_IPC_TXF_NO_SUCH_USER;
	expect("TX_FAILED for TX_UNITDATA with a conn_id", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_CONN_ID);
	r = base_txf; r.b[OFF_BODY + 19] = 1;
	expect("TX_FAILED reserved", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_RESERVED);
	{
		struct aworker_ipc_tx_failed f = { .ref_type = AWORKER_IPC_MT_TX_DISCONN, .conn_id = 1,
						   .disposition = AWORKER_IPC_TXF_NOT_EXECUTED,
						   .code = AWORKER_IPC_TXF_DATA_TOO_LONG };
		CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 1, &f) == -EINVAL);
		f.disposition = AWORKER_IPC_TXF_EXECUTED_ERROR;
		f.code = AWORKER_IPC_TXF_LIB_ERROR;
		f.lib_rc = -5;
		f.post_conn = AWORKER_IPC_POST_CONN_PRESENT;
		CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 1, &f) == 0);
		msgb_free(msg);
		f.ref_type = AWORKER_IPC_MT_TX_UNITDATA;
		f.conn_id = AWORKER_IPC_CONN_ID_NONE;
		CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 1, &f) == -EINVAL);	/* post_conn for unitdata */
		f.post_conn = AWORKER_IPC_POST_CONN_NA;
		CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, 1, &f) == 0);
		msgb_free(msg);
		printf("  TX_FAILED encoder enforces the same contract\n");
	}

	/* GOODBYE / BIND_RESULT values */
	CHECK(aworker_ipc_enc_goodbye(ctx, &msg, AWORKER_IPC_GOODBYE_SHUTDOWN) == 0);
	rec_from(&r, msg);
	r.b[OFF_BODY] = 2;
	expect("GOODBYE reason 2", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
	CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, 1, AWORKER_IPC_BIND_OK) == 0);
	rec_from(&r, msg);
	r.b[OFF_BODY + 1] = 4;
	expect("USER_BIND_RESULT 4", &r, AWORKER_IPC_ROLE_MAIN, AWORKER_IPC_ERR_BAD_VALUE);
}

/***********************************************************************
 * C6: deterministic mutation sweep: every record the decoder accepts re-encodes to the identical bytes
 ***********************************************************************/

static uint32_t lcg_state = 0x12345678;
static uint32_t lcg(void)
{
	lcg_state = lcg_state * 1103515245 + 12345;
	return lcg_state >> 8;
}

static struct msgb *data_from(const struct aworker_ipc_data *d)
{
	struct msgb *msg = msgb_alloc_c(ctx, 4096, "re-encode data");
	if (d->len)
		memcpy(msgb_put(msg, d->len), d->buf, d->len);
	return msg;
}

/* Encode a decoded message again with the public encoders */
static int reencode(const struct aworker_ipc_msg *m, struct msgb **out)
{
	const typeof(m->u) *u = &m->u;
	int rc;

	switch (m->type) {
	case AWORKER_IPC_MT_HELLO:
		rc = aworker_ipc_enc_hello(ctx, out, u->hello.role, u->hello.worker_idx, u->hello.pid, u->hello.ident);
		if (rc == 0) {	/* the encoder always writes our own limits */
			uint8_t *b = msgb_data(*out) + OFF_BODY;
			b[4] = u->hello.max_frame >> 8; b[5] = u->hello.max_frame & 0xff;
			b[6] = u->hello.max_data >> 8; b[7] = u->hello.max_data & 0xff;
		}
		break;
	case AWORKER_IPC_MT_ECHO_REQ:
	case AWORKER_IPC_MT_ECHO_RSP:
		rc = aworker_ipc_enc_echo(ctx, out, m->type == AWORKER_IPC_MT_ECHO_RSP, u->echo_token);
		break;
	case AWORKER_IPC_MT_GOODBYE:
		rc = aworker_ipc_enc_goodbye(ctx, out, u->goodbye_reason);
		break;
	case AWORKER_IPC_MT_USER_BIND:
		rc = aworker_ipc_enc_user_bind(ctx, out, m->user_ref, u->user_bind.cs7_id, u->user_bind.ssn,
					       u->user_bind.name);
		break;
	case AWORKER_IPC_MT_USER_UNBIND:
		rc = aworker_ipc_enc_user_unbind(ctx, out, m->user_ref);
		break;
	case AWORKER_IPC_MT_USER_BIND_RESULT:
		rc = aworker_ipc_enc_user_bind_result(ctx, out, m->user_ref, u->bind_result);
		break;
	case AWORKER_IPC_MT_TX_UNITDATA:
		rc = aworker_ipc_enc_tx_unitdata(ctx, out, m->user_ref, &u->tx_unitdata.calling, &u->tx_unitdata.called,
						 data_from(&m->data));
		break;
	case AWORKER_IPC_MT_TX_CONN_REQ:
		rc = aworker_ipc_enc_tx_conn_req(ctx, out, m->user_ref, u->tx_conn_req.conn_id,
						 u->tx_conn_req.calling_present ? &u->tx_conn_req.calling : NULL,
						 &u->tx_conn_req.called, data_from(&m->data));
		break;
	case AWORKER_IPC_MT_TX_CONN_RESP:
		rc = aworker_ipc_enc_tx_conn_resp(ctx, out, m->user_ref, u->tx_conn_resp.conn_id, &u->tx_conn_resp.resp,
						  m->data.buf, m->data.len);
		break;
	case AWORKER_IPC_MT_TX_DATA:
		rc = aworker_ipc_enc_tx_data(ctx, out, m->user_ref, u->tx_data.conn_id, data_from(&m->data));
		break;
	case AWORKER_IPC_MT_TX_DISCONN:
		rc = aworker_ipc_enc_tx_disconn(ctx, out, m->user_ref, u->tx_disconn.conn_id,
						u->tx_disconn.resp_present ? &u->tx_disconn.resp : NULL,
						u->tx_disconn.cause);
		break;
	case AWORKER_IPC_MT_SCU_UP:
		rc = aworker_ipc_enc_scu_up(ctx, out, m->user_ref, &u->scu_up, m->data.buf, m->data.len);
		break;
	case AWORKER_IPC_MT_TX_FAILED:
		rc = aworker_ipc_enc_tx_failed(ctx, out, m->user_ref, &u->tx_failed);
		break;
	default:
		rc = -EINVAL;
	}
	if (rc == 0)
		aworker_ipc_frame_set_seq(msgb_data(*out), m->seq);
	return rc;
}

static void test_c6_mutation_sweep(void)
{
	struct rec seeds[16];
	unsigned int n_seeds = 0, i;
	unsigned int accepted = 0, rejected = 0, mismatch = 0;
	struct osmo_sccp_addr a_gt, a_ip6, a_ip4;
	struct osmo_scu_prim p;
	struct msgb *msg;
	size_t blocks;

	printf("C6: mutation sweep\n");
	memset(seeds, 0, sizeof(seeds));	/* records may grow into the zeroed tail */
	addr_gt(&a_gt);
	addr_ip6(&a_ip6);
	addr_ip4(&a_ip4);

#define SEED(expr) do { CHECK((expr) == 0); rec_from(&seeds[n_seeds++], msg); } while (0)
	SEED(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_WORKER, 2, 77, "osmo-bsc 1.15"));
	SEED(aworker_ipc_enc_echo(ctx, &msg, false, 5));
	SEED(aworker_ipc_enc_user_bind(ctx, &msg, 3, 0, 254, "msc-0"));
	SEED(aworker_ipc_enc_user_bind_result(ctx, &msg, 3, AWORKER_IPC_BIND_OK));
	SEED(aworker_ipc_enc_tx_unitdata(ctx, &msg, 3, &a_gt, &a_ip6, data_msgb(20, 1)));
	SEED(aworker_ipc_enc_tx_conn_req(ctx, &msg, 3, 9, &a_ip4, &a_gt, data_msgb(30, 2)));
	SEED(aworker_ipc_enc_tx_conn_req(ctx, &msg, 3, 9, NULL, &a_ip6, data_msgb(0, 2)));
	SEED(aworker_ipc_enc_tx_conn_resp(ctx, &msg, 3, 9, &a_ip4, (const uint8_t *)"abc", 3));
	SEED(aworker_ipc_enc_tx_data(ctx, &msg, 3, 9, data_msgb(10, 3)));
	SEED(aworker_ipc_enc_tx_disconn(ctx, &msg, 3, 9, &a_gt, 1));
	scu_prim_init(&p, OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION);
	fill_up_params(&p);
	SEED(aworker_ipc_enc_scu_up(ctx, &msg, 3, &p, (const uint8_t *)"xyz", 3));
	scu_prim_init(&p, OSMO_SCU_PRIM_N_STATE, PRIM_OP_INDICATION);
	fill_up_params(&p);
	SEED(aworker_ipc_enc_scu_up(ctx, &msg, 3, &p, NULL, 0));
	{
		struct aworker_ipc_tx_failed f = { .ref_type = AWORKER_IPC_MT_TX_CONN_REQ, .conn_id = 9,
						   .disposition = AWORKER_IPC_TXF_NOT_EXECUTED,
						   .code = AWORKER_IPC_TXF_CONN_ID_IN_USE };
		SEED(aworker_ipc_enc_tx_failed(ctx, &msg, 3, &f));
	}
#undef SEED

	blocks = talloc_total_blocks(ctx);
	for (i = 0; i < 200000; i++) {
		struct rec r = seeds[lcg() % n_seeds];
		unsigned int nmut = 1 + lcg() % 3, k;
		struct aworker_ipc_msg m;
		enum aworker_ipc_role role = (lcg() & 1) ? AWORKER_IPC_ROLE_MAIN : AWORKER_IPC_ROLE_WORKER;

		for (k = 0; k < nmut; k++) {
			uint32_t pos = lcg() % r.len;
			switch (lcg() % 4) {
			case 0: r.b[pos] ^= 1 << (lcg() % 8); break;
			case 1: r.b[pos] = lcg(); break;
			case 2: r.b[pos] = 0; break;
			case 3: r.b[pos] += 1; break;
			}
		}
		if (lcg() % 8 == 0)	/* sometimes also change the length */
			r.len = OSMO_MIN(r.len + (lcg() % 5), sizeof(r.b)) - (lcg() % 5);

		if (aworker_ipc_dec(&m, r.b, r.len, role) != 0) {
			rejected++;
			continue;
		}
		accepted++;
		if (reencode(&m, &msg) != 0) {
			mismatch++;
			continue;
		}
		if (msgb_length(msg) != r.len || memcmp(msgb_data(msg), r.b, r.len))
			mismatch++;
		msgb_free(msg);
	}
	printf("  200000 mutated records: %u accepted, %u rejected, %u not canonical\n", accepted, rejected,
	       mismatch);
	CHECK(mismatch == 0);
	CHECK(talloc_total_blocks(ctx) == blocks);
}

/***********************************************************************
 * C11: CS7_INFO (the metadata a worker reports about the cs7 instances it owns)
 ***********************************************************************/

static void cs7_info_fill(struct aworker_ipc_cs7_info *info, unsigned int count)
{
	unsigned int i;

	memset(info, 0, sizeof(*info));
	info->count = count;
	for (i = 0; i < count; i++) {
		info->e[i].cs7_id = i;
		info->e[i].primary_pc = 0x00112200 + i;
		info->e[i].asp_proto = (i % 2) ? OSMO_SS7_ASP_PROT_IPA : OSMO_SS7_ASP_PROT_M3UA;
		info->e[i].num_as = (uint8_t)(i + 1);
	}
}

static void test_c11_cs7_info(void)
{
	struct aworker_ipc_cs7_info info;
	struct aworker_ipc_msg m;
	struct msgb *msg = NULL;
	uint8_t raw[AWORKER_IPC_MAX_FRAME];
	unsigned int count, len, i;
	int rc;

	printf("C11: CS7_INFO\n");

	/* Round trip for every valid number of entries. */
	for (count = 1; count <= AWORKER_IPC_MAX_CS7; count++) {
		cs7_info_fill(&info, count);
		CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == 0);
		if (!msg)
			continue;
		CHECK(dec(&m, msg, AWORKER_IPC_ROLE_MAIN) == 0);
		CHECK(m.type == AWORKER_IPC_MT_CS7_INFO);
		/* Worker-scoped, not SCCP-User scoped: user_ref must be 0. */
		CHECK(m.user_ref == 0);
		CHECK(m.u.cs7_info.count == count);
		CHECK(m.data.len == 0);
		for (i = 0; i < count; i++) {
			CHECK(m.u.cs7_info.e[i].cs7_id == info.e[i].cs7_id);
			CHECK(m.u.cs7_info.e[i].primary_pc == info.e[i].primary_pc);
			CHECK(m.u.cs7_info.e[i].asp_proto == info.e[i].asp_proto);
			CHECK(m.u.cs7_info.e[i].num_as == info.e[i].num_as);
		}
		/* 16 octet header + 1 octet count + 12 octets per entry */
		CHECK(msgb_length(msg) == AWORKER_IPC_HDR_LEN + 1 + 12 * count);
		printf("  count=%u: %u octets, round trip ok\n", count, msgb_length(msg));
		msgb_free(msg);
		msg = NULL;
	}

	/* Direction: only the main process may receive it. */
	cs7_info_fill(&info, 1);
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == 0);
	CHECK(dec(&m, msg, AWORKER_IPC_ROLE_WORKER) == -AWORKER_IPC_ERR_WRONG_DIRECTION);
	CHECK(aworker_ipc_type_valid_for(AWORKER_IPC_MT_CS7_INFO, AWORKER_IPC_ROLE_MAIN));
	CHECK(!aworker_ipc_type_valid_for(AWORKER_IPC_MT_CS7_INFO, AWORKER_IPC_ROLE_WORKER));
	printf("  direction: worker->main only\n");

	/* Golden vector, so that a change of the wire format cannot go unnoticed. */
	aworker_ipc_frame_set_seq(msgb_data(msg), 2);
	print_hex("CS7_INFO 1 entry", msgb_data(msg), msgb_length(msg));
	len = msgb_length(msg);
	memcpy(raw, msgb_data(msg), len);
	msgb_free(msg);
	msg = NULL;

	/* Values the encoder must refuse. */
	cs7_info_fill(&info, 1);
	info.count = 0;
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 1);
	info.count = AWORKER_IPC_MAX_CS7 + 1;
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 1);
	info.e[0].asp_proto = OSMO_SS7_ASP_PROT_NONE;
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 1);
	info.e[0].asp_proto = _NUM_OSMO_SS7_ASP_PROT;
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 1);
	info.e[0].num_as = 0;
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 1);
	info.e[0].primary_pc = 0x01000000;	/* more than 24 bit */
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	cs7_info_fill(&info, 2);
	info.e[1].cs7_id = info.e[0].cs7_id;	/* a cs7 instance has exactly one owner */
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == -EINVAL && msg == NULL);
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, NULL) == -EINVAL && msg == NULL);
	printf("  encoder refuses: count 0 and %u, proto none and %u, num_as 0, pc > 24 bit,"
	       " duplicate cs7 id, NULL\n", AWORKER_IPC_MAX_CS7 + 1, _NUM_OSMO_SS7_ASP_PROT);

	/* The same violations on the wire, plus the structural ones, must be rejected by the decoder. */
	{
		static const struct { const char *name; unsigned int off; uint8_t val; int err; } bad[] = {
			{ "count 0", 16, 0, AWORKER_IPC_ERR_BAD_VALUE },
			{ "count too large", 16, AWORKER_IPC_MAX_CS7 + 1, AWORKER_IPC_ERR_BAD_VALUE },
			{ "asp_proto none", 25, OSMO_SS7_ASP_PROT_NONE, AWORKER_IPC_ERR_BAD_VALUE },
			{ "asp_proto out of range", 25, _NUM_OSMO_SS7_ASP_PROT, AWORKER_IPC_ERR_BAD_VALUE },
			{ "num_as 0", 26, 0, AWORKER_IPC_ERR_BAD_VALUE },
			{ "reserved[0] set", 27, 1, AWORKER_IPC_ERR_BAD_RESERVED },
			{ "reserved[1] set", 28, 1, AWORKER_IPC_ERR_BAD_RESERVED },
			{ "primary_pc > 24 bit", 21, 0x01, AWORKER_IPC_ERR_BAD_VALUE },
		};
		uint8_t buf[AWORKER_IPC_MAX_FRAME];
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(bad); k++) {
			memcpy(buf, raw, len);
			buf[bad[k].off] = bad[k].val;
			rc = aworker_ipc_dec(&m, buf, len, AWORKER_IPC_ROLE_MAIN);
			CHECK(rc == -bad[k].err);
			if (rc != -bad[k].err)
				printf("  !! %s: got %s, expected %s\n", bad[k].name, aworker_ipc_err_name(rc),
				       aworker_ipc_err_name(bad[k].err));
		}
		printf("  decoder rejects %u field violations\n", (unsigned int)ARRAY_SIZE(bad));

		/* A truncated record, a record with trailing octets, and a non-zero user_ref. */
		memcpy(buf, raw, len);
		buf[7] = (uint8_t)(len - AWORKER_IPC_HDR_LEN - 1);
		CHECK(aworker_ipc_dec(&m, buf, len - 1, AWORKER_IPC_ROLE_MAIN) == -AWORKER_IPC_ERR_TRUNCATED);
		memcpy(buf, raw, len);
		buf[7] = (uint8_t)(len - AWORKER_IPC_HDR_LEN + 1);
		buf[len] = 0;
		CHECK(aworker_ipc_dec(&m, buf, len + 1, AWORKER_IPC_ROLE_MAIN) == -AWORKER_IPC_ERR_TRAILING_DATA);
		memcpy(buf, raw, len);
		buf[15] = 1;	/* user_ref */
		CHECK(aworker_ipc_dec(&m, buf, len, AWORKER_IPC_ROLE_MAIN) == -AWORKER_IPC_ERR_BAD_USER_REF);
		printf("  decoder rejects: truncated body, trailing octets, non-zero user_ref\n");
	}
}

/***********************************************************************
 * C10: per-operation SCCP data limits
 ***********************************************************************/

static void test_c10_sccp_limits(void)
{
	static const struct {
		uint8_t type;
		const char *name;
		int max;
	} lim[] = {
		{ AWORKER_IPC_MT_TX_UNITDATA, "TX_UNITDATA", 1139 },
		{ AWORKER_IPC_MT_TX_CONN_REQ, "TX_CONN_REQ", 255 },
		{ AWORKER_IPC_MT_TX_CONN_RESP, "TX_CONN_RESP", 255 },
		{ AWORKER_IPC_MT_TX_DATA, "TX_DATA", 255 },
		{ AWORKER_IPC_MT_TX_DISCONN, "TX_DISCONN", 0 },
	};
	/* Largest SCCP address as osmo_sccp_addr_encode() writes it: indicator, PC, SSN, TT + NP/ES + NAI, 31 BCD digits */
	const unsigned int addr_max = sizeof(struct sccp_called_party_address) + 2 + 1 + 3 + 16;
	const unsigned int ludt_overhead = sizeof(struct sccp_data_long_unitdata) + 2 * (1 + addr_max) + 2;
	struct msgb *ipa;
	unsigned int i;
	int max;

	printf("C10: libosmo-sigtran per-operation data limits\n");
	for (i = 0; i < ARRAY_SIZE(lim); i++) {
		int rc_max, rc_over;
		max = aworker_ipc_sccp_max_data(lim[i].type);
		rc_max = aworker_ipc_sccp_check_data(lim[i].type, max);
		rc_over = aworker_ipc_sccp_check_data(lim[i].type, max + 1);
		printf("  %s: max %d; %d: %s, %d: %s\n", lim[i].name, max, max, errno_name(rc_max), max + 1,
		       errno_name(rc_over));
		CHECK(max == lim[i].max && rc_max == 0 && rc_over == -EMSGSIZE);
		CHECK(max <= AWORKER_IPC_MAX_DATA);
	}
	printf("  HELLO: %s, %s\n", errno_name(aworker_ipc_sccp_max_data(AWORKER_IPC_MT_HELLO)),
	       errno_name(aworker_ipc_sccp_check_data(AWORKER_IPC_MT_HELLO, 0)));
	CHECK(aworker_ipc_sccp_max_data(AWORKER_IPC_MT_HELLO) == -EINVAL);
	CHECK(aworker_ipc_sccp_check_data(AWORKER_IPC_MT_HELLO, 0) == -EINVAL);

	/* Cross-check the TX_UNITDATA limit against the real IPA transmit buffer (libosmocore ipa_msg_alloc(), as used
	 * by libosmo-sigtran ipa_to_msg()) and the public SCCP LUDT layout: the maximum fits exactly, one more does not.
	 * (The M3UA buffer is private to libosmo-sigtran and its bound, 1443, is not binding.) */
	max = aworker_ipc_sccp_max_data(AWORKER_IPC_MT_TX_UNITDATA);
	ipa = ipa_msg_alloc(16);
	OSMO_ASSERT(ipa);
	printf("  IPA transmit buffer: %d octets; SCCP LUDT overhead with two %u octet addresses: %u octets\n",
	       msgb_tailroom(ipa), addr_max, ludt_overhead);
	CHECK(max + ludt_overhead == (unsigned int)msgb_tailroom(ipa));
	msgb_free(ipa);

	/* N-DATA / CR / CC user data: one length octet in a DT1, SCCP_MAX_DATA (256) is the library's own ceiling */
	CHECK(aworker_ipc_sccp_max_data(AWORKER_IPC_MT_TX_DATA) == SCCP_MAX_DATA - 1);
	CHECK(aworker_ipc_sccp_max_data(AWORKER_IPC_MT_TX_DATA) <= 255);
	/* the helper's own buffer never limits before the transmit path does */
	CHECK(max + sizeof(struct osmo_scu_prim) <= 2048);
}

/***********************************************************************
 * Link tests
 ***********************************************************************/

struct tl {
	const char *name;
	struct aworker_ipc_link *link;
	int fd;
	unsigned int ready;
	unsigned int closed;
	struct aworker_ipc_link_close_info ci;
	unsigned int rx;
	struct aworker_ipc_msg last;	/* DATA views not valid after rx_cb */
	unsigned int last_data_len;
	uint8_t last_data0;
	uint32_t expect_next_data_seq;	/* for the ordering test: first data octet of TX_DATA */
	bool order_ok;
	bool free_in_rx;
	bool close_in_rx;
	bool free_in_closed;
};

static void tl_ready(struct aworker_ipc_link *link, const struct aworker_ipc_hello *peer)
{
	struct tl *t = aworker_ipc_link_get_priv(link);
	t->ready++;
}

static void tl_rx(struct aworker_ipc_link *link, const struct aworker_ipc_msg *msg)
{
	struct tl *t = aworker_ipc_link_get_priv(link);
	t->rx++;
	t->last = *msg;
	t->last_data_len = msg->data.len;
	t->last_data0 = msg->data.len ? msg->data.buf[0] : 0;
	if (msg->type == AWORKER_IPC_MT_TX_DATA && msg->data.len >= 4) {
		uint32_t v = osmo_load32be(msg->data.buf);
		if (v != t->expect_next_data_seq)
			t->order_ok = false;
		t->expect_next_data_seq = v + 1;
	}
	if (t->close_in_rx)
		aworker_ipc_link_close(link);
	if (t->free_in_rx) {
		aworker_ipc_link_free(link);
		t->link = NULL;
	}
}

static void tl_closed(struct aworker_ipc_link *link, const struct aworker_ipc_link_close_info *info)
{
	struct tl *t = aworker_ipc_link_get_priv(link);
	t->closed++;
	t->ci = *info;
	if (t->free_in_closed) {
		aworker_ipc_link_free(link);
		t->link = NULL;
	}
}

static const struct aworker_ipc_link_ops tl_ops = {
	.ready_cb = tl_ready,
	.rx_cb = tl_rx,
	.closed_cb = tl_closed,
};

static int tl_alloc(struct tl *t, const char *name, int fd, enum aworker_ipc_role role, uint16_t idx,
		    const char *ident, unsigned int txqueue_max)
{
	struct aworker_ipc_link_cfg cfg = {
		.role = role, .worker_idx = idx, .pid = 1000 + role, .ident = ident, .txqueue_max = txqueue_max,
		.log_cat = DLGLOBAL,
	};
	memset(t, 0, sizeof(*t));
	t->name = name;
	t->fd = fd;
	t->order_ok = true;
	return aworker_ipc_link_alloc(&t->link, ctx, fd, &cfg, &tl_ops, t);
}

static void tl_free(struct tl *t)
{
	if (t->link)
		aworker_ipc_link_free(t->link);
	t->link = NULL;
}

static void pump(unsigned int n)
{
	while (n--)
		osmo_select_main(1);
}

#define PUMP_UNTIL(cond) do { \
		unsigned int _i; \
		for (_i = 0; _i < 2000 && !(cond); _i++) \
			osmo_select_main(1); \
	} while (0)

static void mk_pair(int sv[2])
{
	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
}

static void print_close(const struct tl *t)
{
	char err[64];

	switch (t->ci.reason) {
	case AWORKER_IPC_CLOSE_PROTOCOL:
		snprintf(err, sizeof(err), "%s", aworker_ipc_err_name(t->ci.err));
		break;
	case AWORKER_IPC_CLOSE_RX_ERROR:
	case AWORKER_IPC_CLOSE_TX_ERROR:
	case AWORKER_IPC_CLOSE_TX_QUEUE_FULL:
		snprintf(err, sizeof(err), "%s", t->ci.err == -EPIPE ? "-EPIPE" : t->ci.err == -EMSGSIZE ? "-EMSGSIZE"
			 : t->ci.err == -ENOSPC ? "-ENOSPC" : t->ci.err == -ECONNRESET ? "-ECONNRESET" : "(other)");
		break;
	default:
		snprintf(err, sizeof(err), "%d", t->ci.err);
		break;
	}
	printf("  %s closed %u time(s): %s, err=%s, last_written_seq=%u, last_queued_seq=%u\n", t->name, t->closed,
	       aworker_ipc_close_reason_name(t->ci.reason), err, t->ci.last_written_seq, t->ci.last_queued_seq);
}

static struct msgb *tx_data_rec(uint32_t user_ref, uint32_t conn_id, unsigned int len, uint8_t seed)
{
	struct msgb *msg;
	OSMO_ASSERT(aworker_ipc_enc_tx_data(ctx, &msg, user_ref, conn_id, data_msgb(len, seed)) == 0);
	return msg;
}

/* A connected and handshaked main (sv[0]) / worker (sv[1]) pair */
static void ready_pair(struct tl *m, struct tl *w, int sv[2], unsigned int txqueue_max)
{
	mk_pair(sv);
	OSMO_ASSERT(tl_alloc(m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 3, "test", txqueue_max) == 0);
	OSMO_ASSERT(tl_alloc(w, "worker", sv[1], AWORKER_IPC_ROLE_WORKER, 3, "test", 0) == 0);
	PUMP_UNTIL(m->ready && w->ready);
	OSMO_ASSERT(m->ready == 1 && w->ready == 1);
}

static void test_l1_l2_handshake_and_traffic(void)
{
	struct tl m, w;
	int sv[2];
	size_t blocks = talloc_total_blocks(ctx);
	uint32_t seq = 0;
	struct osmo_scu_prim p;
	struct msgb *msg;
	int rc;

	printf("L1/L2: handshake, send gating, traffic, ECHO\n");
	mk_pair(sv);
	CHECK(tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 3, "osmo-bsc A", 0) == 0);
	CHECK(tl_alloc(&w, "worker", sv[1], AWORKER_IPC_ROLE_WORKER, 3, "osmo-bsc B", 0) == 0);
	CHECK(aworker_ipc_link_get_state(m.link) == AWORKER_IPC_LINK_HELLO_WAIT);

	rc = aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL);
	printf("  send before handshake: %s\n", errno_name(rc));
	CHECK(rc == -EBUSY);

	PUMP_UNTIL(m.ready && w.ready);
	printf("  main ready=%u, worker ready=%u (idents differ: warning only)\n", m.ready, w.ready);
	CHECK(m.ready == 1 && w.ready == 1);
	CHECK(aworker_ipc_link_get_state(m.link) == AWORKER_IPC_LINK_READY);

	rc = aworker_ipc_link_send(m.link, tx_data_rec(7, 99, 5, 0x40), &seq);
	CHECK(rc == 0);
	PUMP_UNTIL(w.rx == 1);
	printf("  main->worker TX_DATA: seq=%u, worker rx=%u, conn_id=%u, len=%u, data[0]=0x%02x\n", seq, w.rx,
	       w.last.u.tx_data.conn_id, w.last_data_len, w.last_data0);
	CHECK(w.last.type == AWORKER_IPC_MT_TX_DATA && w.last.user_ref == 7 && w.last.seq == 2);

	scu_prim_init(&p, OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION);
	p.u.data.conn_id = 99;
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 7, &p, (const uint8_t *)"\x01\x02", 2) == 0);
	rc = aworker_ipc_link_send(w.link, msg, &seq);
	CHECK(rc == 0);
	PUMP_UNTIL(m.rx == 1);
	printf("  worker->main SCU_UP: seq=%u, main rx=%u, primitive=%u, conn_id=%u\n", seq, m.rx,
	       m.last.u.scu_up.oph.primitive, m.last.u.scu_up.u.data.conn_id);
	CHECK(m.last.type == AWORKER_IPC_MT_SCU_UP && m.last_data_len == 2);

	CHECK(aworker_ipc_enc_echo(ctx, &msg, false, 0xabcd) == 0);
	CHECK(aworker_ipc_link_send(m.link, msg, NULL) == 0);
	PUMP_UNTIL(m.rx == 2);
	printf("  main ECHO_REQ answered by the worker link: main rx type=%s token=0x%x, worker rx=%u\n",
	       aworker_ipc_msg_type_name(m.last.type), m.last.u.echo_token, w.rx);
	CHECK(m.last.type == AWORKER_IPC_MT_ECHO_RSP && m.last.u.echo_token == 0xabcd && w.rx == 1);

	/* records that the peer would reject are refused locally; the link stays up */
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 7, &p, NULL, 0) == 0);
	rc = aworker_ipc_link_send(m.link, msg, NULL);
	printf("  main sending SCU_UP (wrong direction): %s, state ready: %s\n", errno_name(rc),
	       aworker_ipc_link_get_state(m.link) == AWORKER_IPC_LINK_READY ? "yes" : "no");
	CHECK(rc == -EINVAL);
	CHECK(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_MAIN, 3, 0, "") == 0);
	rc = aworker_ipc_link_send(m.link, msg, NULL);
	printf("  main sending a HELLO itself: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);
	msg = msgb_alloc_c(ctx, 32, "garbage");
	msgb_put_u32(msg, 0x12345678);
	rc = aworker_ipc_link_send(m.link, msg, NULL);
	printf("  main sending 4 garbage octets: %s\n", errno_name(rc));
	CHECK(rc == -EINVAL);

	{
		struct aworker_ipc_link_stats st;
		aworker_ipc_link_get_stats(m.link, &st);
		printf("  main stats: rx=%u tx_queued=%u tx_written=%u last_written_seq=%u\n", st.rx_records,
		       st.tx_queued, st.tx_written, st.last_written_seq);
		CHECK(st.tx_queued == st.tx_written && st.last_written_seq == 3);
	}

	CHECK(m.closed == 0 && w.closed == 0);
	tl_free(&m);
	tl_free(&w);
	pump(5);
	printf("  talloc blocks after free: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAK");
	CHECK(talloc_total_blocks(ctx) == blocks);
}

/* Raw peer helpers */
static void raw_send(int fd, struct msgb *msg, uint32_t seq)
{
	aworker_ipc_frame_set_seq(msgb_data(msg), seq);
	OSMO_ASSERT(send(fd, msgb_data(msg), msgb_length(msg), 0) == (ssize_t)msgb_length(msg));
	msgb_free(msg);
}

static void raw_send_bytes(int fd, const uint8_t *buf, size_t len)
{
	OSMO_ASSERT(send(fd, buf, len, 0) == (ssize_t)len);
}

static struct msgb *raw_hello(enum aworker_ipc_role role, uint16_t idx)
{
	struct msgb *msg;
	OSMO_ASSERT(aworker_ipc_enc_hello(ctx, &msg, role, idx, 1, "raw") == 0);
	return msg;
}

/* After the link closed, the raw peer sees the link's HELLO and then EOF */
static void raw_check_eof(int fd)
{
	uint8_t buf[AWORKER_IPC_MAX_FRAME];
	ssize_t n;
	int records = 0;
	while ((n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0)
		records++;
	printf("  raw peer: %d record(s) then %s\n", records, n == 0 ? "EOF" : "no EOF");
	CHECK(n == 0);
}

static void raw_case(const char *what, void (*inject)(int fd))
{
	struct tl m;
	int sv[2];
	size_t blocks = talloc_total_blocks(ctx);

	printf(" %s\n", what);
	mk_pair(sv);
	CHECK(tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 3, "test", 0) == 0);
	inject(sv[1]);
	PUMP_UNTIL(m.closed);
	print_close(&m);
	CHECK(m.closed == 1 && m.rx == 0);
	raw_check_eof(sv[1]);
	tl_free(&m);
	close(sv[1]);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);
}

static void inj_role_main(int fd) { raw_send(fd, raw_hello(AWORKER_IPC_ROLE_MAIN, 3), 1); }
static void inj_idx4(int fd) { raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 4), 1); }
static void inj_limits(int fd)
{
	struct msgb *msg = raw_hello(AWORKER_IPC_ROLE_WORKER, 3);
	msgb_data(msg)[OFF_BODY + 7] = 0x00;	/* max_data 0x0800 -> 0x0000 */
	msgb_data(msg)[OFF_BODY + 6] = 0x00;
	raw_send(fd, msg, 1);
}
static void inj_no_hello(int fd)
{
	struct msgb *msg;
	OSMO_ASSERT(aworker_ipc_enc_echo(ctx, &msg, false, 1) == 0);
	raw_send(fd, msg, 1);
}
static void inj_hello_seq2(int fd) { raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 2); }
static void inj_version2(int fd)
{
	struct msgb *msg = raw_hello(AWORKER_IPC_ROLE_WORKER, 3);
	msgb_data(msg)[2] = 2;
	raw_send(fd, msg, 1);
}
static void inj_dup_hello(int fd)
{
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 2);
}
static void inj_seq_gap(int fd)
{
	struct msgb *msg;
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	OSMO_ASSERT(aworker_ipc_enc_echo(ctx, &msg, true, 1) == 0);
	raw_send(fd, msg, 3);
}
static void inj_oversize_4097(int fd)
{
	static uint8_t buf[AWORKER_IPC_MAX_FRAME + 1];
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	raw_send_bytes(fd, buf, sizeof(buf));
}
static void inj_oversize_6000(int fd)
{
	static uint8_t buf[6000];
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	raw_send_bytes(fd, buf, sizeof(buf));
}
static void inj_garbage_4096(int fd)
{
	static uint8_t buf[AWORKER_IPC_MAX_FRAME];
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	memset(buf, 0x41, sizeof(buf));
	raw_send_bytes(fd, buf, sizeof(buf));
}
static void inj_wrong_direction(int fd)
{
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	raw_send(fd, tx_data_rec(1, 1, 1, 0), 2);	/* TX_DATA towards main */
}
static void inj_eof_after_hello(int fd)
{
	raw_send(fd, raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	shutdown(fd, SHUT_WR);
}

static void test_l3_l5_raw_peer(void)
{
	printf("L3-L5: incompatible and malformed peers (raw socket peer)\n");
	raw_case("peer HELLO with the same role", inj_role_main);
	raw_case("peer HELLO with worker index 4", inj_idx4);
	raw_case("peer HELLO with different limits", inj_limits);
	raw_case("first record is not a HELLO", inj_no_hello);
	raw_case("HELLO with seq 2", inj_hello_seq2);
	raw_case("HELLO with version 2", inj_version2);
	raw_case("second HELLO", inj_dup_hello);
	raw_case("sequence gap", inj_seq_gap);
	raw_case("4097 octet record", inj_oversize_4097);
	raw_case("6000 octet record (truncated by the kernel)", inj_oversize_6000);
	raw_case("4096 octet malformed record", inj_garbage_4096);
	raw_case("record for the wrong direction", inj_wrong_direction);
	raw_case("peer shuts down after its HELLO", inj_eof_after_hello);
}

static void test_l6_l7_size_and_order(void)
{
	struct tl m, w;
	int sv[2];
	size_t blocks = talloc_total_blocks(ctx);
	struct osmo_scu_prim p;
	struct msgb *msg;
	unsigned int i, len;

	printf("L6/L7: largest valid record, 1000 records in order\n");
	ready_pair(&m, &w, sv, 0);

	scu_prim_init(&p, OSMO_SCU_PRIM_N_CONNECT, PRIM_OP_INDICATION);
	fill_up_params(&p);
	{
		struct msgb *d = data_msgb(AWORKER_IPC_MAX_DATA, 0x77);
		CHECK(aworker_ipc_enc_scu_up(ctx, &msg, 1, &p, msgb_data(d), msgb_length(d)) == 0);
		msgb_free(d);
	}
	len = msgb_length(msg);
	CHECK(aworker_ipc_link_send(w.link, msg, NULL) == 0);
	PUMP_UNTIL(m.rx == 1);
	printf("  largest valid record (SCU_UP N-CONNECT.ind + %u octets): %u octets, received intact: %s\n",
	       AWORKER_IPC_MAX_DATA, len,
	       (m.rx == 1 && m.last_data_len == AWORKER_IPC_MAX_DATA && m.last_data0 == 0x77) ? "yes" : "no");
	CHECK(m.rx == 1 && m.last_data_len == AWORKER_IPC_MAX_DATA);

	w.expect_next_data_seq = 0;
	for (i = 0; i < 1000; i++) {
		struct msgb *d = msgb_alloc_c(ctx, 4096, "order");
		msgb_put_u32(d, i);
		memset(msgb_put(d, i % 300), 0x5a, i % 300);
		CHECK(aworker_ipc_enc_tx_data(ctx, &msg, 1, 1, d) == 0);
		CHECK(aworker_ipc_link_send(m.link, msg, NULL) == 0);
		if (i % 100 == 99)
			pump(3);
	}
	PUMP_UNTIL(w.rx == 1000);
	{
		struct aworker_ipc_link_stats st;
		aworker_ipc_link_get_stats(m.link, &st);
		printf("  1000 records: worker rx=%u, in order: %s, main tx_queued=%u tx_written=%u\n", w.rx,
		       w.order_ok ? "yes" : "no", st.tx_queued, st.tx_written);
		CHECK(w.rx == 1000 && w.order_ok && st.tx_queued == st.tx_written);
	}
	CHECK(m.closed == 0 && w.closed == 0);
	tl_free(&m);
	tl_free(&w);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);
}

static void test_l8_l12_lifecycle(void)
{
	struct tl m, w;
	int sv[2];
	size_t blocks = talloc_total_blocks(ctx);
	unsigned int i;
	int rc;

	printf("L8/L9: peer EOF, send after close\n");
	ready_pair(&m, &w, sv, 0);
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	PUMP_UNTIL(w.rx == 1);
	tl_free(&w);	/* closes the worker end without closed_cb */
	PUMP_UNTIL(m.closed);
	print_close(&m);
	CHECK(m.closed == 1 && m.ci.reason == AWORKER_IPC_CLOSE_PEER_EOF && m.ci.last_written_seq == 2);
	CHECK(w.closed == 0);
	rc = aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL);
	printf("  send after close: %s\n", errno_name(rc));
	CHECK(rc == -ENOTCONN);
	aworker_ipc_link_close(m.link);
	CHECK(m.closed == 1);
	printf("  aworker_ipc_link_close() on a closed link: closed_cb not called again\n");
	tl_free(&m);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);

	printf("L10: transmit queue full\n");
	ready_pair(&m, &w, sv, 4);
	for (i = 0; i < 4; i++)
		CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 10, 0), NULL) == 0);
	rc = aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 10, 0), NULL);
	printf("  5th record with a queue of 4: %s\n", errno_name(rc));
	CHECK(rc == -ENOSPC);
	print_close(&m);
	CHECK(m.closed == 1 && m.ci.reason == AWORKER_IPC_CLOSE_TX_QUEUE_FULL && m.ci.last_queued_seq == 5);
	PUMP_UNTIL(w.closed);
	printf("  worker received %u of the 4 queued records, then: %s\n", w.rx,
	       aworker_ipc_close_reason_name(w.ci.reason));
	CHECK(w.rx == 0 && w.ci.reason == AWORKER_IPC_CLOSE_PEER_EOF);
	tl_free(&m);
	tl_free(&w);
	pump(2);
	printf("  talloc blocks after free: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAK");
	CHECK(talloc_total_blocks(ctx) == blocks);

	printf("L11: close and free from inside call-backs\n");
	ready_pair(&m, &w, sv, 0);
	w.free_in_rx = true;
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	PUMP_UNTIL(m.closed);
	printf("  worker freed itself in rx_cb: worker rx=%u, closed_cb=%u; main: %s (%s)\n", w.rx, w.closed,
	       aworker_ipc_close_reason_name(m.ci.reason), m.ci.err == -ECONNRESET ? "ECONNRESET" : "?");
	/* the worker end was closed with an unread record queued: Linux reports ECONNRESET to the peer */
	CHECK(w.rx == 1 && w.closed == 0 && w.link == NULL);
	CHECK(m.ci.reason == AWORKER_IPC_CLOSE_RX_ERROR && m.ci.err == -ECONNRESET);
	tl_free(&m);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);

	ready_pair(&m, &w, sv, 0);
	w.close_in_rx = true;
	w.free_in_closed = true;
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	PUMP_UNTIL(m.closed);
	printf("  worker closed in rx_cb and freed in closed_cb: worker rx=%u, closed_cb=%u (%s); main: %s (%s)\n",
	       w.rx, w.closed, aworker_ipc_close_reason_name(w.ci.reason), aworker_ipc_close_reason_name(m.ci.reason),
	       m.ci.err == -ECONNRESET ? "ECONNRESET" : "?");
	CHECK(w.rx == 1 && w.closed == 1 && w.ci.reason == AWORKER_IPC_CLOSE_LOCAL && w.link == NULL);
	CHECK(m.ci.reason == AWORKER_IPC_CLOSE_RX_ERROR && m.ci.err == -ECONNRESET);
	tl_free(&m);
	pump(2);
	printf("  talloc blocks after free: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAK");
	CHECK(talloc_total_blocks(ctx) == blocks);

	printf("L12: SIGPIPE\n");
	mk_pair(sv);
	signal(SIGPIPE, SIG_DFL);
	rc = tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 3, "test", 0);
	printf("  alloc with SIGPIPE not ignored: %s, fd still open: %s\n", errno_name(rc),
	       fcntl(sv[0], F_GETFD) != -1 ? "yes" : "no");
	CHECK(rc == -EPIPE && fcntl(sv[0], F_GETFD) != -1);
	signal(SIGPIPE, SIG_IGN);
	CHECK(tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 3, "test", 0) == 0);
	raw_send(sv[1], raw_hello(AWORKER_IPC_ROLE_WORKER, 3), 1);
	PUMP_UNTIL(m.ready);
	shutdown(sv[1], SHUT_RD);	/* the peer no longer reads: our next send fails with EPIPE */
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 1, 0), NULL) == 0);
	PUMP_UNTIL(m.closed);
	print_close(&m);
	CHECK(m.closed == 1 && m.ci.reason == AWORKER_IPC_CLOSE_TX_ERROR && m.ci.err == -EPIPE);
	CHECK(m.ci.last_written_seq == 1 && m.ci.last_queued_seq == 2);
	tl_free(&m);
	close(sv[1]);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);
}

static void test_l13_fd_types(void)
{
	struct tl m;
	int sv[2], fd, rc;

	printf("L13: unsuitable file descriptors\n");
	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	rc = tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 0, "", 0);
	printf("  AF_UNIX SOCK_STREAM: %s, fd open: %s\n", errno_name(rc), fcntl(sv[0], F_GETFD) != -1 ? "yes" : "no");
	CHECK(rc == -EPROTOTYPE && fcntl(sv[0], F_GETFD) != -1);
	close(sv[0]);
	close(sv[1]);

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	OSMO_ASSERT(fd >= 0);
	rc = tl_alloc(&m, "main", fd, AWORKER_IPC_ROLE_MAIN, 0, "", 0);
	printf("  AF_INET SOCK_DGRAM: %s, fd open: %s\n", errno_name(rc), fcntl(fd, F_GETFD) != -1 ? "yes" : "no");
	CHECK(rc == -EPROTOTYPE && fcntl(fd, F_GETFD) != -1);
	close(fd);

	OSMO_ASSERT(pipe(sv) == 0);
	rc = tl_alloc(&m, "main", sv[0], AWORKER_IPC_ROLE_MAIN, 0, "", 0);
	printf("  pipe: %s, fd open: %s\n", errno_name(rc), fcntl(sv[0], F_GETFD) != -1 ? "yes" : "no");
	CHECK(rc == -ENOTSOCK && fcntl(sv[0], F_GETFD) != -1);
	close(sv[0]);
	close(sv[1]);

	OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0);
	{
		struct aworker_ipc_link_cfg cfg = { .role = 7 };
		struct aworker_ipc_link *l;
		rc = aworker_ipc_link_alloc(&l, ctx, sv[0], &cfg, &tl_ops, NULL);
		printf("  invalid role: %s\n", errno_name(rc));
		CHECK(rc == -EINVAL);
		cfg.role = AWORKER_IPC_ROLE_MAIN;
		cfg.ident = "tab\tident";
		rc = aworker_ipc_link_alloc(&l, ctx, sv[0], &cfg, &tl_ops, NULL);
		printf("  ident with a control character: %s\n", errno_name(rc));
		CHECK(rc == -EINVAL && fcntl(sv[0], F_GETFD) != -1);
	}
	close(sv[0]);
	close(sv[1]);
}

/* L14-L19: send failures injected below the real osmo_io */
static void inj_case(const char *what, enum inj_action action)
{
	struct tl m, w;
	int sv[2];
	size_t blocks = talloc_total_blocks(ctx);
	uint32_t seq;
	unsigned int w_rx_before;

	printf(" %s\n", what);
	ready_pair(&m, &w, sv, 0);
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 3, 0x10), NULL) == 0);
	PUMP_UNTIL(w.rx == 1);
	w_rx_before = w.rx;

	inj.fd = sv[0];
	inj.calls = 0;
	inj.injected = 0;
	inj.action = action;
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 3, 0x20), &seq) == 0);
	CHECK(aworker_ipc_link_send(m.link, tx_data_rec(1, 1, 3, 0x30), NULL) == 0);
	PUMP_UNTIL(m.closed || w.rx == w_rx_before + 2);
	pump(5);
	PUMP_UNTIL(w.closed || w.rx == w_rx_before + 2);
	printf("  injected=%u, sendmsg() calls on the fd=%u, record seq %u\n", inj.injected, inj.calls, seq);
	if (m.closed)
		print_close(&m);
	else
		printf("  main still ready\n");
	printf("  worker received %u of the 2 records\n", w.rx - w_rx_before);
	if (w.rx > w_rx_before)
		printf("  worker last record: seq=%u data[0]=0x%02x\n", w.last.seq, w.last_data0);
	if (w.closed)
		print_close(&w);

	switch (action) {
	case INJ_SHORT:
		/* closed in sendmsg_cb: no further sendmsg() call, so the remainder was never sent */
		CHECK(m.closed == 1 && m.ci.reason == AWORKER_IPC_CLOSE_TX_PARTIAL);
		CHECK(inj.calls == 1 && m.ci.last_written_seq == seq - 1 && m.ci.last_queued_seq == seq + 1);
		CHECK(w.rx == w_rx_before && w.ci.reason == AWORKER_IPC_CLOSE_PEER_EOF);
		break;
	case INJ_EMSGSIZE:
		CHECK(m.closed == 1 && m.ci.reason == AWORKER_IPC_CLOSE_TX_ERROR && m.ci.err == -EMSGSIZE);
		CHECK(inj.calls == 1 && m.ci.last_written_seq == seq - 1);
		CHECK(w.rx == w_rx_before && w.ci.reason == AWORKER_IPC_CLOSE_PEER_EOF);
		break;
	case INJ_ZERO:
	case INJ_EAGAIN:
		/* osmo_io re-queued the complete record: both arrive once, intact and in order */
		CHECK(m.closed == 0 && w.closed == 0);
		CHECK(inj.calls == 3 && w.rx == w_rx_before + 2 && w.last.seq == seq + 1 && w.last_data0 == 0x30);
		break;
	default:
		break;
	}
	inj.fd = -1;
	tl_free(&m);
	tl_free(&w);
	pump(2);
	CHECK(talloc_total_blocks(ctx) == blocks);
}

static void test_l14_l19_injection(void)
{
	const char *backend = getenv("LIBOSMO_IO_BACKEND");

	printf("L14-L19: send failures injected below osmo_io\n");
	if (backend && strcmp(backend, "POLL")) {
		printf("  SKIPPED: requires the poll backend (LIBOSMO_IO_BACKEND=%s)\n", backend);
		return;
	}
	inj_case("L14: sendmsg() accepts all but one octet", INJ_SHORT);
	inj_case("L15: sendmsg() returns 0 once", INJ_ZERO);
	inj_case("L16: sendmsg() fails with EAGAIN once", INJ_EAGAIN);
	inj_case("L17: sendmsg() fails with EMSGSIZE", INJ_EMSGSIZE);
	printf("  L19: fault injection active: %s\n", inj.injected ? "yes" : "NO");
	CHECK(inj.injected > 0);
}

/***********************************************************************/

static const struct log_info_cat test_log_cat[] = {
	[0] = { .name = "DTEST", .description = "test", .enabled = 1, .loglevel = LOGL_DEBUG },
};

static const struct log_info test_log_info = {
	.cat = test_log_cat,
	.num_cat = ARRAY_SIZE(test_log_cat),
};

int main(int argc, char **argv)
{
	size_t blocks;

	ctx = talloc_named_const(NULL, 0, "aworker_ipc_test");
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	log_set_print_category(osmo_stderr_target, 0);
	log_set_print_category_hex(osmo_stderr_target, 0);
	log_set_print_level(osmo_stderr_target, 1);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_category_filter(osmo_stderr_target, DLGLOBAL, 1, LOGL_DEBUG);
	signal(SIGPIPE, SIG_IGN);
	blocks = talloc_total_blocks(ctx);

	test_c1_downlink_roundtrip();
	test_c2_uplink_roundtrip();
	test_c3_golden();
	test_c4_boundaries();
	test_c5_malformed();
	test_c6_mutation_sweep();
	test_c10_sccp_limits();
	test_c11_cs7_info();
	test_l1_l2_handshake_and_traffic();
	test_l3_l5_raw_peer();
	test_l6_l7_size_and_order();
	test_l8_l12_lifecycle();
	test_l13_fd_types();
	test_l14_l19_injection();

	printf("talloc blocks at the end: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAK");
	CHECK(talloc_total_blocks(ctx) == blocks);
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

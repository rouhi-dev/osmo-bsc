/* Tests for the main-process side of the A-worker pool (aworker_pool.c) */
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

/* The pool is exercised as the production code: real cs7 instances, the real IPC links on real
 * socketpair()s, the real libosmocore osmo_io event loop, and the real worker-backed SCCP User backend
 * (bsc_sccp_backend_worker_ops) reached through bsc_sccp_tx_*(). Only the worker processes are replaced: this
 * test plays the worker end of each link by hand, with the same codec, through aworker_pool_start_with_fds().
 *
 * What is checked: the cs7 instance -> worker routing, the bind handshake and its effect on the users, every
 * transmit operation as it appears on the wire, the up-call path, the TX_FAILED reaction, the data limits, and
 * what happens when a worker goes away or misbehaves. */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <osmocom/core/application.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/select.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/sccp_sap.h>

#include <osmocom/bsc/aworker_ipc.h>
#include <osmocom/bsc/aworker_pool.h>
#include <osmocom/bsc/bsc_sccp_backend.h>
#include <osmocom/bsc/debug.h>
#include <osmocom/bsc/gsm_data.h>

void *tall_bsc_ctx;

static void *ctx;
static unsigned int failures;

#define CHECK(exp) do { \
		if (!(exp)) { \
			printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #exp); \
			failures++; \
		} \
	} while (0)

static const struct log_info_cat test_cat[] = {
	[DAWORKER] = { .name = "DAWORKER", .description = "A-worker pool", .enabled = 1,
		       .loglevel = LOGL_NOTICE },
};
static const struct log_info test_log_info = { .cat = test_cat, .num_cat = ARRAY_SIZE(test_cat) };

/* Run the event loop a few times, so that queued records are written and received ones are processed. */
static void pump(void)
{
	unsigned int i;
	for (i = 0; i < 6; i++)
		osmo_select_main(1);
}

/***********************************************************************
 * The worker end of a link, played by hand
 ***********************************************************************/

struct fake_worker {
	int fd;				/* worker end of the socketpair */
	uint16_t idx;
	uint32_t tx_seq;		/* next sequence number we send */
	uint32_t rx_seq;		/* sequence number we expect next */
	/*! Receive buffer. A decoded message points its DATA view into the buffer it was decoded from, so the
	 * buffer must outlive the struct aworker_ipc_msg the caller inspects -- exactly as the link keeps the
	 * received record alive for the duration of its rx_cb. */
	uint8_t rxbuf[AWORKER_IPC_MAX_FRAME + 1];
};

static void fw_send(struct fake_worker *fw, struct msgb *msg)
{
	ssize_t rc;

	OSMO_ASSERT(msg);
	aworker_ipc_frame_set_seq(msgb_data(msg), fw->tx_seq++);
	rc = send(fw->fd, msgb_data(msg), msgb_length(msg), 0);
	CHECK(rc == (ssize_t)msgb_length(msg));
	msgb_free(msg);
}

/* Receive one record, or return false if none is pending. */
static bool fw_recv(struct fake_worker *fw, struct aworker_ipc_msg *m)
{
	ssize_t len;
	int rc;

	len = recv(fw->fd, fw->rxbuf, sizeof(fw->rxbuf), MSG_DONTWAIT);
	if (len <= 0)
		return false;
	rc = aworker_ipc_dec(m, fw->rxbuf, (size_t)len, AWORKER_IPC_ROLE_WORKER);
	CHECK(rc == 0);
	if (rc < 0)
		return false;
	CHECK(m->seq == fw->rx_seq);
	fw->rx_seq++;
	return true;
}

/* Receive the next record and check its type. */
static bool fw_expect(struct fake_worker *fw, uint8_t type, struct aworker_ipc_msg *m)
{
	if (!fw_recv(fw, m)) {
		printf("  FAIL worker %u: expected %s, nothing received\n", fw->idx,
		       aworker_ipc_msg_type_name(type));
		failures++;
		return false;
	}
	if (m->type != type) {
		printf("  FAIL worker %u: expected %s, got %s\n", fw->idx, aworker_ipc_msg_type_name(type),
		       aworker_ipc_msg_type_name(m->type));
		failures++;
		return false;
	}
	return true;
}

static bool fw_idle(struct fake_worker *fw)
{
	struct aworker_ipc_msg m;
	if (!fw_recv(fw, &m))
		return true;
	printf("  FAIL worker %u: unexpected %s\n", fw->idx, aworker_ipc_msg_type_name(m.type));
	failures++;
	return false;
}

/* The main side queues its HELLO for every worker when the pool starts; read it and answer it. */
static void fw_hello(struct fake_worker *fw)
{
	struct aworker_ipc_msg m;
	struct msgb *msg;

	CHECK(fw_expect(fw, AWORKER_IPC_MT_HELLO, &m));
	CHECK(m.u.hello.role == AWORKER_IPC_ROLE_MAIN);
	CHECK(m.u.hello.worker_idx == fw->idx);
	CHECK(m.u.hello.max_frame == AWORKER_IPC_MAX_FRAME && m.u.hello.max_data == AWORKER_IPC_MAX_DATA);
	CHECK(aworker_ipc_enc_hello(ctx, &msg, AWORKER_IPC_ROLE_WORKER, fw->idx, 4242, "test-worker") == 0);
	fw_send(fw, msg);
}

/* Report the cs7 instances this worker owns; the pool only routes to a worker that has done this. */
static void fw_cs7_info(struct fake_worker *fw, const uint32_t *cs7_ids, unsigned int num_cs7)
{
	struct aworker_ipc_cs7_info info = {};
	struct msgb *msg;
	unsigned int i;

	info.count = num_cs7;
	for (i = 0; i < num_cs7; i++) {
		info.e[i].cs7_id = cs7_ids[i];
		info.e[i].primary_pc = 0x17 + cs7_ids[i];
		info.e[i].asp_proto = OSMO_SS7_ASP_PROT_M3UA;
		info.e[i].num_as = 1;
	}
	CHECK(aworker_ipc_enc_cs7_info(ctx, &msg, &info) == 0);
	fw_send(fw, msg);
}

static void fw_handshake(struct fake_worker *fw, const uint32_t *cs7_ids, unsigned int num_cs7)
{
	fw_hello(fw);
	fw_cs7_info(fw, cs7_ids, num_cs7);
}

/***********************************************************************
 * The SCCP user the pool binds for us
 ***********************************************************************/

struct user_state {
	const char *name;
	struct bsc_sccp_user *user;
	struct bsc_sccp_inst *bsc_sccp;
	unsigned int available;
	unsigned int unavailable;
	unsigned int prims;
	unsigned int tx_failed;
	/* last up-call */
	unsigned int last_prim;
	unsigned int last_op;
	uint32_t last_conn_id;
	unsigned int last_len;
	uint8_t last_data0;
	/* last TX_FAILED */
	struct aworker_ipc_tx_failed last_f;
};

static struct user_state us0, us1;

static struct user_state *us_of(struct bsc_sccp_user *user)
{
	if (us0.user == user)
		return &us0;
	if (us1.user == user)
		return &us1;
	OSMO_ASSERT(0);
}

static int test_prim_cb(struct bsc_sccp_user *user, struct osmo_prim_hdr *oph)
{
	struct user_state *u = us_of(user);
	struct osmo_scu_prim *prim = (struct osmo_scu_prim *)oph;

	u->prims++;
	u->last_prim = oph->primitive;
	u->last_op = oph->operation;
	u->last_conn_id = AWORKER_IPC_CONN_ID_NONE;
	switch (oph->primitive) {
	case OSMO_SCU_PRIM_N_CONNECT:
		u->last_conn_id = prim->u.connect.conn_id;
		break;
	case OSMO_SCU_PRIM_N_DATA:
		u->last_conn_id = prim->u.data.conn_id;
		break;
	case OSMO_SCU_PRIM_N_DISCONNECT:
		u->last_conn_id = prim->u.disconnect.conn_id;
		break;
	default:
		break;
	}
	u->last_len = oph->msg && msgb_l2(oph->msg) ? msgb_l2len(oph->msg) : 0;
	u->last_data0 = u->last_len ? ((uint8_t *)msgb_l2(oph->msg))[0] : 0;
	/* Same contract as the in-process backend: we own the msgb. */
	msgb_free(oph->msg);
	return 0;
}

static void test_available(struct bsc_sccp_user *user)
{
	us_of(user)->available++;
}

static void test_unavailable(struct bsc_sccp_user *user)
{
	us_of(user)->unavailable++;
}

static void test_tx_failed(struct bsc_sccp_user *user, const struct aworker_ipc_tx_failed *f)
{
	struct user_state *u = us_of(user);
	u->tx_failed++;
	u->last_f = *f;
}

static const struct aworker_sccp_user_ops test_ops = {
	.tx_failed = test_tx_failed,
	.unavailable = test_unavailable,
	.available = test_available,
};

/***********************************************************************
 * Fixture
 ***********************************************************************/

#define NUM_WORKERS	2
static struct fake_worker fw[NUM_WORKERS];
static struct osmo_ss7_instance *ss7[NUM_WORKERS];

static void addr_pc(struct osmo_sccp_addr *a, uint32_t pc, uint32_t ssn)
{
	memset(a, 0, sizeof(*a));
	a->presence = OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN;
	a->ri = OSMO_SCCP_RI_SSN_PC;
	a->pc = pc;
	a->ssn = ssn;
}

static struct msgb *data_msgb(unsigned int len, uint8_t seed)
{
	struct msgb *msg = msgb_alloc_c(ctx, OSMO_MAX(len, 1) + 16, "test data");
	unsigned int i;

	OSMO_ASSERT(msg);
	for (i = 0; i < len; i++)
		msgb_put_u8(msg, (uint8_t)(seed + i));
	return msg;
}

static void fixture_up(void)
{
	int fds[NUM_WORKERS];
	unsigned int i;

	for (i = 0; i < NUM_WORKERS; i++) {
		int sv[2];
		OSMO_ASSERT(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
		fds[i] = sv[0];
		memset(&fw[i], 0, sizeof(fw[i]));
		fw[i].fd = sv[1];
		fw[i].idx = (uint16_t)i;
		fw[i].tx_seq = 1;
		fw[i].rx_seq = 1;
		ss7[i] = osmo_ss7_instance_find_or_create(ctx, i);
		OSMO_ASSERT(ss7[i]);
	}
	CHECK(aworker_pool_start_with_fds(ctx, fds, NUM_WORKERS) == 0);
}

static void us_free(struct user_state *u)
{
	if (u->user)
		aworker_sccp_user_unbind(u->user);
	u->user = NULL;
	talloc_free(u->bsc_sccp);
	u->bsc_sccp = NULL;
}

static void fixture_down(void)
{
	unsigned int i;

	aworker_pool_stop();
	for (i = 0; i < NUM_WORKERS; i++) {
		if (fw[i].fd >= 0)
			close(fw[i].fd);
		fw[i].fd = -1;
	}
	for (i = 0; i < NUM_WORKERS; i++) {
		if (ss7[i])
			osmo_ss7_instance_destroy(ss7[i]);
		ss7[i] = NULL;
	}
}

/***********************************************************************
 * T1: bind handshake and the cs7 instance -> worker routing
 ***********************************************************************/

static void test_t1_bind_and_routing(void)
{
	static const uint32_t cs7_of_w0[] = { 0 };
	static const uint32_t cs7_of_w1[] = { 1 };
	struct aworker_ipc_msg m;
	struct msgb *msg;

	printf("T1: bind handshake and cs7 instance -> worker routing\n");
	fixture_up();

	/* Users are registered before any worker is ready: nothing can be sent yet. */
	us0 = (struct user_state){ .name = "msc-0" };
	us1 = (struct user_state){ .name = "msc-1" };
	us0.bsc_sccp = bsc_sccp_inst_alloc(ctx);
	us1.bsc_sccp = bsc_sccp_inst_alloc(ctx);
	us0.user = aworker_sccp_user_bind(ctx, ss7[0], us0.bsc_sccp, us0.name, test_prim_cb,
					  OSMO_SCCP_SSN_BSSAP, &test_ops);
	us1.user = aworker_sccp_user_bind(ctx, ss7[1], us1.bsc_sccp, us1.name, test_prim_cb,
					  OSMO_SCCP_SSN_BSSAP, &test_ops);
	CHECK(us0.user && us1.user);
	CHECK(us0.user->ops == &bsc_sccp_backend_worker_ops);
	CHECK(bsc_sccp_user_get_sccp(us0.user) == NULL);
	CHECK(bsc_sccp_user_get_ss7(us0.user) == ss7[0]);
	CHECK(bsc_sccp_user_get_bsc_sccp(us0.user) == us0.bsc_sccp);
	CHECK(!aworker_sccp_user_ready(us0.user));
	CHECK(bsc_sccp_tx_data_msg(us0.user, 1, data_msgb(4, 0)) == -ENOTCONN);
	printf("  before the handshake: not ready, transmit gives -ENOTCONN\n");

	/* A second user on the same cs7 instance and SSN is refused; find() returns the first. */
	CHECK(aworker_sccp_user_bind(ctx, ss7[0], us0.bsc_sccp, "dup", test_prim_cb, OSMO_SCCP_SSN_BSSAP,
				     &test_ops) == NULL);
	CHECK(aworker_sccp_user_find(0, OSMO_SCCP_SSN_BSSAP) == us0.user);
	CHECK(aworker_sccp_user_find(1, OSMO_SCCP_SSN_BSSAP) == us1.user);
	CHECK(aworker_sccp_user_find(2, OSMO_SCCP_SSN_BSSAP) == NULL);
	CHECK(aworker_sccp_user_find(0, OSMO_SCCP_SSN_SMLC_BSSAP_LE) == NULL);
	printf("  duplicate bind refused, find() by cs7 instance and SSN works\n");

	pump();
	/* Both links complete their handshake, but only worker 1 reports its cs7 instances: the routing
	 * follows the CS7_INFO reports, not the worker index or the handshake order. */
	fw_hello(&fw[0]);
	fw_hello(&fw[1]);
	pump();
	CHECK(fw_idle(&fw[0]) && fw_idle(&fw[1]));
	CHECK(!aworker_pool_all_ready());
	CHECK(!aworker_sccp_user_ready(us0.user) && !aworker_sccp_user_ready(us1.user));
	printf("  handshake alone does not bind anything: no USER_BIND before CS7_INFO\n");

	fw_cs7_info(&fw[1], cs7_of_w1, 1);
	pump();
	CHECK(!aworker_pool_all_ready());
	/* Only the user of cs7 instance 1 can be bound so far. */
	CHECK(fw_expect(&fw[1], AWORKER_IPC_MT_USER_BIND, &m));
	CHECK(m.u.user_bind.cs7_id == 1);
	CHECK(m.u.user_bind.ssn == OSMO_SCCP_SSN_BSSAP);
	CHECK(!strcmp(m.u.user_bind.name, "msc-1"));
	CHECK(m.user_ref != 0);
	CHECK(fw_idle(&fw[0]));
	printf("  worker 1 ready: USER_BIND for cs7 instance 1 only (user_ref %u)\n", m.user_ref);

	CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, m.user_ref, AWORKER_IPC_BIND_OK) == 0);
	fw_send(&fw[1], msg);
	pump();
	CHECK(us1.available == 1 && us1.unavailable == 0);
	CHECK(aworker_sccp_user_ready(us1.user));
	CHECK(!aworker_sccp_user_ready(us0.user));
	printf("  bind result OK: user 'msc-1' available, 'msc-0' still waiting\n");

	fw_cs7_info(&fw[0], cs7_of_w0, 1);
	pump();
	CHECK(aworker_pool_all_ready());
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_USER_BIND, &m));
	CHECK(m.u.user_bind.cs7_id == 0);
	CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, m.user_ref, AWORKER_IPC_BIND_OK) == 0);
	fw_send(&fw[0], msg);
	pump();
	CHECK(us0.available == 1 && aworker_sccp_user_ready(us0.user));
	printf("  worker 0 ready: both users bound, pool reports all workers ready\n");

	/* What the worker reported is readable back. */
	{
		struct aworker_ipc_cs7_entry e;
		CHECK(aworker_pool_cs7_info(0, &e));
		CHECK(e.cs7_id == 0 && e.primary_pc == 0x17 && e.asp_proto == OSMO_SS7_ASP_PROT_M3UA);
		CHECK(aworker_pool_cs7_info(1, &e));
		CHECK(e.cs7_id == 1 && e.primary_pc == 0x18);
		CHECK(!aworker_pool_cs7_info(7, &e));
		printf("  CS7_INFO of both workers is available to the A interface\n");
	}
}

/***********************************************************************
 * T2: every transmit operation, as it appears on the worker's end
 ***********************************************************************/

static void test_t2_transmit(void)
{
	struct osmo_sccp_addr calling, called, resp;
	struct aworker_ipc_msg m;
	uint8_t co_data[8];
	unsigned int i;

	printf("T2: transmit operations\n");
	addr_pc(&calling, 0x17, OSMO_SCCP_SSN_BSSAP);
	addr_pc(&called, 0x18, OSMO_SCCP_SSN_BSSAP);
	addr_pc(&resp, 0x19, OSMO_SCCP_SSN_BSSAP);
	for (i = 0; i < sizeof(co_data); i++)
		co_data[i] = (uint8_t)(0xa0 + i);

	CHECK(bsc_sccp_tx_unitdata_msg(us0.user, &calling, &called, data_msgb(20, 0x10)) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_UNITDATA, &m));
	CHECK(!memcmp(&m.u.tx_unitdata.calling, &calling, sizeof(calling)));
	CHECK(!memcmp(&m.u.tx_unitdata.called, &called, sizeof(called)));
	CHECK(m.data.len == 20 && m.data.buf[0] == 0x10);
	printf("  TX_UNITDATA: addresses and %u octets of data\n", m.data.len);

	CHECK(bsc_sccp_tx_conn_req_msg(us0.user, 0x101, &calling, &called, data_msgb(30, 0x20)) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_CONN_REQ, &m));
	CHECK(m.u.tx_conn_req.conn_id == 0x101);
	CHECK(m.u.tx_conn_req.calling_present);
	CHECK(!memcmp(&m.u.tx_conn_req.calling, &calling, sizeof(calling)));
	CHECK(m.data.len == 30 && m.data.buf[0] == 0x20);
	printf("  TX_CONN_REQ: conn_id %u, calling address present\n", m.u.tx_conn_req.conn_id);

	/* No calling address: the optional address must be reported absent. */
	CHECK(bsc_sccp_tx_conn_req_msg(us0.user, 0x102, NULL, &called, data_msgb(0, 0)) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_CONN_REQ, &m));
	CHECK(m.u.tx_conn_req.conn_id == 0x102 && !m.u.tx_conn_req.calling_present && m.data.len == 0);
	printf("  TX_CONN_REQ without a calling address, empty data\n");

	CHECK(bsc_sccp_tx_conn_resp(us0.user, 0x103, &resp, co_data, sizeof(co_data)) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_CONN_RESP, &m));
	CHECK(m.u.tx_conn_resp.conn_id == 0x103);
	CHECK(!memcmp(&m.u.tx_conn_resp.resp, &resp, sizeof(resp)));
	CHECK(m.data.len == sizeof(co_data) && m.data.buf[0] == 0xa0);
	printf("  TX_CONN_RESP: conn_id %u, responding address, %u octets\n", m.u.tx_conn_resp.conn_id,
	       m.data.len);

	CHECK(bsc_sccp_tx_data_msg(us0.user, 0x104, data_msgb(40, 0x30)) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_DATA, &m));
	CHECK(m.u.tx_data.conn_id == 0x104 && m.data.len == 40 && m.data.buf[0] == 0x30);
	printf("  TX_DATA: conn_id %u, %u octets\n", m.u.tx_data.conn_id, m.data.len);

	CHECK(bsc_sccp_tx_disconn(us0.user, 0x105, &resp, 0x42) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_DISCONN, &m));
	CHECK(m.u.tx_disconn.conn_id == 0x105 && m.u.tx_disconn.cause == 0x42);
	CHECK(m.u.tx_disconn.resp_present);
	printf("  TX_DISCONN: conn_id %u, cause %u\n", m.u.tx_disconn.conn_id, m.u.tx_disconn.cause);

	CHECK(bsc_sccp_tx_disconn(us0.user, 0x106, NULL, 0) == 0);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_DISCONN, &m));
	CHECK(!m.u.tx_disconn.resp_present);
	printf("  TX_DISCONN without a responding address\n");

	/* Routing: the user of cs7 instance 1 is served by worker 1, and worker 0 sees nothing. */
	CHECK(bsc_sccp_tx_data_msg(us1.user, 0x201, data_msgb(5, 0x50)) == 0);
	pump();
	CHECK(fw_idle(&fw[0]));
	CHECK(fw_expect(&fw[1], AWORKER_IPC_MT_TX_DATA, &m));
	CHECK(m.u.tx_data.conn_id == 0x201 && m.data.len == 5 && m.data.buf[0] == 0x50);
	printf("  a user of cs7 instance 1 is served by worker 1, not worker 0\n");
	CHECK(fw_idle(&fw[0]) && fw_idle(&fw[1]));
}

/***********************************************************************
 * T3: data limits of the A interface are enforced before anything is sent
 ***********************************************************************/

static void test_t3_limits(void)
{
	struct osmo_sccp_addr calling, called;
	size_t blocks;
	int max;

	printf("T3: per-operation data limits\n");
	addr_pc(&calling, 0x17, OSMO_SCCP_SSN_BSSAP);
	addr_pc(&called, 0x18, OSMO_SCCP_SSN_BSSAP);
	blocks = talloc_total_blocks(ctx);

	max = aworker_ipc_sccp_max_data(AWORKER_IPC_MT_TX_UNITDATA);
	CHECK(bsc_sccp_tx_unitdata_msg(us0.user, &calling, &called, data_msgb(max, 1)) == 0);
	pump();
	{
		struct aworker_ipc_msg m;
		CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_UNITDATA, &m));
		CHECK(m.data.len == (unsigned int)max);
	}
	CHECK(bsc_sccp_tx_unitdata_msg(us0.user, &calling, &called, data_msgb(max + 1, 1)) == -EMSGSIZE);
	CHECK(fw_idle(&fw[0]));
	printf("  TX_UNITDATA: %d octets accepted, %d rejected with -EMSGSIZE\n", max, max + 1);

	max = aworker_ipc_sccp_max_data(AWORKER_IPC_MT_TX_DATA);
	CHECK(bsc_sccp_tx_data_msg(us0.user, 7, data_msgb(max, 1)) == 0);
	pump();
	{
		struct aworker_ipc_msg m;
		CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_DATA, &m));
		CHECK(m.data.len == (unsigned int)max);
	}
	CHECK(bsc_sccp_tx_data_msg(us0.user, 7, data_msgb(max + 1, 1)) == -EMSGSIZE);
	CHECK(bsc_sccp_tx_conn_req_msg(us0.user, 8, &calling, &called, data_msgb(max + 1, 1)) == -EMSGSIZE);
	CHECK(bsc_sccp_tx_conn_resp(us0.user, 9, &called, (const uint8_t *)"x", max + 1) == -EMSGSIZE);
	CHECK(fw_idle(&fw[0]));
	printf("  connection-oriented: %d octets accepted, %d rejected on all three operations\n", max, max + 1);

	/* The rejected msgbs were consumed, exactly as the libosmo-sigtran helpers do it. */
	CHECK(talloc_total_blocks(ctx) == blocks);
	printf("  rejected transmit requests free their msgb: talloc blocks %s\n",
	       talloc_total_blocks(ctx) == blocks ? "unchanged" : "LEAKED");
}

/***********************************************************************
 * T4: the up-call path and TX_FAILED
 ***********************************************************************/

static void test_t4_uplink(void)
{
	struct osmo_scu_prim p;
	struct msgb *msg;
	uint8_t data[6] = { 0x90, 1, 2, 3, 4, 5 };

	printf("T4: SCU_UP and TX_FAILED\n");

	/* N-DATA.ind reaches the user's prim_cb, with the data and the connection id. */
	memset(&p, 0, sizeof(p));
	osmo_prim_init(&p.oph, SCCP_SAP_USER, OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION, NULL);
	p.u.data.conn_id = 0x301;
	CHECK(aworker_ipc_enc_scu_up(ctx, &msg, aworker_sccp_user_ready(us0.user) ? 1 : 1, &p, data,
				     sizeof(data)) == 0);
	/* The user_ref must be the one the pool assigned; take it from the USER_BIND we answered. */
	msgb_free(msg);
	{
		/* Re-derive the live user_ref by provoking a TX and reading it back from the wire. */
		struct aworker_ipc_msg m;
		uint32_t user_ref;

		CHECK(bsc_sccp_tx_data_msg(us0.user, 0x300, data_msgb(1, 0)) == 0);
		pump();
		CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_TX_DATA, &m));
		user_ref = m.user_ref;

		CHECK(aworker_ipc_enc_scu_up(ctx, &msg, user_ref, &p, data, sizeof(data)) == 0);
		fw_send(&fw[0], msg);
		pump();
		CHECK(us0.prims == 1);
		CHECK(us0.last_prim == OSMO_SCU_PRIM_N_DATA && us0.last_op == PRIM_OP_INDICATION);
		CHECK(us0.last_conn_id == 0x301);
		CHECK(us0.last_len == sizeof(data) && us0.last_data0 == 0x90);
		printf("  N-DATA.ind delivered to the user: conn_id %u, %u octets\n", us0.last_conn_id,
		       us0.last_len);

		/* N-DISCONNECT.ind with no user data. */
		memset(&p, 0, sizeof(p));
		osmo_prim_init(&p.oph, SCCP_SAP_USER, OSMO_SCU_PRIM_N_DISCONNECT, PRIM_OP_INDICATION, NULL);
		p.u.disconnect.conn_id = 0x301;
		p.u.disconnect.cause = 5;
		CHECK(aworker_ipc_enc_scu_up(ctx, &msg, user_ref, &p, NULL, 0) == 0);
		fw_send(&fw[0], msg);
		pump();
		CHECK(us0.prims == 2 && us0.last_prim == OSMO_SCU_PRIM_N_DISCONNECT && us0.last_len == 0);
		printf("  N-DISCONNECT.ind delivered, no user data\n");

		/* An unknown user_ref is dropped; it must not take the link down. */
		CHECK(aworker_ipc_enc_scu_up(ctx, &msg, user_ref + 0x1000, &p, NULL, 0) == 0);
		fw_send(&fw[0], msg);
		pump();
		CHECK(us0.prims == 2 && us1.prims == 0);
		CHECK(aworker_sccp_user_ready(us0.user));
		printf("  SCU_UP with an unknown user_ref is dropped, the link stays up\n");

		/* TX_FAILED, both dispositions. */
		{
			struct aworker_ipc_tx_failed f = {
				.ref_seq = 7,
				.ref_type = AWORKER_IPC_MT_TX_DATA,
				.disposition = AWORKER_IPC_TXF_NOT_EXECUTED,
				.code = AWORKER_IPC_TXF_CONN_UNKNOWN,
				.conn_id = 0x300,
			};
			CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, user_ref, &f) == 0);
			fw_send(&fw[0], msg);
			pump();
			CHECK(us0.tx_failed == 1);
			CHECK(us0.last_f.disposition == AWORKER_IPC_TXF_NOT_EXECUTED);
			CHECK(us0.last_f.code == AWORKER_IPC_TXF_CONN_UNKNOWN);
			CHECK(us0.last_f.conn_id == 0x300);
			printf("  TX_FAILED NOT_EXECUTED reported to the A interface (conn_id %u)\n",
			       us0.last_f.conn_id);

			f.disposition = AWORKER_IPC_TXF_EXECUTED_ERROR;
			f.code = AWORKER_IPC_TXF_LIB_ERROR;
			f.lib_rc = -1;
			f.post_conn = AWORKER_IPC_POST_CONN_ABSENT;
			CHECK(aworker_ipc_enc_tx_failed(ctx, &msg, user_ref, &f) == 0);
			fw_send(&fw[0], msg);
			pump();
			CHECK(us0.tx_failed == 2);
			CHECK(us0.last_f.disposition == AWORKER_IPC_TXF_EXECUTED_ERROR);
			CHECK(us0.last_f.lib_rc == -1);
			CHECK(us0.last_f.post_conn == AWORKER_IPC_POST_CONN_ABSENT);
			printf("  TX_FAILED EXECUTED_ERROR reported with lib_rc %d (delivery unknown)\n",
			       us0.last_f.lib_rc);
		}
	}
}

/***********************************************************************
 * T5: a worker that goes away, and one that sends something it may not
 ***********************************************************************/

static void test_t5_worker_lost(void)
{
	printf("T5: losing a worker\n");

	/* Worker 1 closes its end: its user loses the worker, the other one is untouched. */
	close(fw[1].fd);
	fw[1].fd = -1;
	pump();
	CHECK(us1.unavailable == 1);
	CHECK(!aworker_sccp_user_ready(us1.user));
	CHECK(bsc_sccp_tx_data_msg(us1.user, 1, data_msgb(4, 0)) == -ENOTCONN);
	CHECK(us0.unavailable == 0 && aworker_sccp_user_ready(us0.user));
	CHECK(!aworker_pool_all_ready());
	printf("  worker 1 gone: its user is unavailable and cannot transmit; worker 0 unaffected\n");

	/* The cs7 instance of a lost worker has no owner any more. */
	{
		struct aworker_ipc_cs7_entry e;
		CHECK(!aworker_pool_cs7_info(1, &e));
		CHECK(aworker_pool_cs7_info(0, &e));
		printf("  cs7 instance 1 has no owner any more, cs7 instance 0 still has one\n");
	}

	/* Worker 0 sends a record the main process may not receive: the link closes (fail closed). */
	{
		struct msgb *msg;
		CHECK(aworker_ipc_enc_user_bind(ctx, &msg, 1, 0, OSMO_SCCP_SSN_BSSAP, "bogus") == 0);
		fw_send(&fw[0], msg);
		pump();
		CHECK(us0.unavailable == 1);
		CHECK(!aworker_sccp_user_ready(us0.user));
		CHECK(bsc_sccp_tx_unitdata_msg(us0.user, NULL, NULL, data_msgb(1, 0)) == -ENOTCONN);
		printf("  a USER_BIND from a worker closes the link: its user is unavailable\n");
	}
	CHECK(us0.unavailable == 1 && us1.unavailable == 1);
	CHECK(us0.available == 1 && us1.available == 1);
	printf("  each 'available' was followed by exactly one 'unavailable'\n");
}

/***********************************************************************
 * T6: unbind and shutdown
 ***********************************************************************/

static void test_t6_unbind_and_stop(void)
{
	printf("T6: unbind and pool shutdown\n");

	aworker_sccp_user_unbind(us1.user);
	us1.user = NULL;
	talloc_free(us1.bsc_sccp);
	us1.bsc_sccp = NULL;
	CHECK(aworker_sccp_user_find(1, OSMO_SCCP_SSN_BSSAP) == NULL);
	printf("  unbind removes the user from the pool\n");

	fixture_down();
	CHECK(aworker_sccp_user_find(0, OSMO_SCCP_SSN_BSSAP) == NULL);
	CHECK(!aworker_pool_all_ready());
	/* A user that outlives the pool must not be usable, and must not crash. */
	CHECK(bsc_sccp_tx_data_msg(us0.user, 1, data_msgb(4, 0)) == -ENOTCONN);
	CHECK(!aworker_sccp_user_ready(us0.user));
	us_free(&us0);
	printf("  after aworker_pool_stop() the users are detached, inert and can be freed\n");
}

/***********************************************************************
 * T7: two workers claiming the same cs7 instance
 ***********************************************************************/

static void test_t7_duplicate_cs7(void)
{
	static const uint32_t same[] = { 0 };
	struct user_state saved0 = us0, saved1 = us1;

	printf("T7: two workers claiming the same cs7 instance\n");
	fixture_up();
	us0 = (struct user_state){ .name = "msc-0" };
	us1 = (struct user_state){ .name = "unused" };
	us0.bsc_sccp = bsc_sccp_inst_alloc(ctx);
	us0.user = aworker_sccp_user_bind(ctx, ss7[0], us0.bsc_sccp, us0.name, test_prim_cb,
					  OSMO_SCCP_SSN_BSSAP, &test_ops);
	CHECK(us0.user != NULL);
	pump();

	fw_handshake(&fw[0], same, 1);
	pump();
	{
		struct aworker_ipc_msg m;
		struct msgb *msg;
		CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_USER_BIND, &m));
		CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, m.user_ref, AWORKER_IPC_BIND_OK) == 0);
		fw_send(&fw[0], msg);
		pump();
		CHECK(aworker_sccp_user_ready(us0.user));
	}

	/* Worker 1 claims cs7 instance 0 as well: its link is closed, worker 0 keeps the instance. */
	fw_handshake(&fw[1], same, 1);
	pump();
	CHECK(aworker_sccp_user_ready(us0.user));
	CHECK(us0.unavailable == 0);
	{
		struct aworker_ipc_cs7_entry e;
		CHECK(aworker_pool_cs7_info(0, &e));
	}
	printf("  the second claim is refused; the first worker keeps cs7 instance 0\n");

	us_free(&us0);
	fixture_down();
	us0 = saved0;
	us1 = saved1;
}

/***********************************************************************
 * T8: a bind the worker refuses
 ***********************************************************************/

static void test_t8_bind_refused(void)
{
	static const uint32_t cs7_0[] = { 0 };
	struct aworker_ipc_msg m;
	struct msgb *msg;

	printf("T8: a bind the worker refuses\n");
	fixture_up();
	us0 = (struct user_state){ .name = "msc-0" };
	us0.bsc_sccp = bsc_sccp_inst_alloc(ctx);
	us0.user = aworker_sccp_user_bind(ctx, ss7[0], us0.bsc_sccp, us0.name, test_prim_cb,
					  OSMO_SCCP_SSN_BSSAP, &test_ops);
	CHECK(us0.user != NULL);
	pump();
	fw_handshake(&fw[0], cs7_0, 1);
	pump();
	CHECK(fw_expect(&fw[0], AWORKER_IPC_MT_USER_BIND, &m));
	CHECK(aworker_ipc_enc_user_bind_result(ctx, &msg, m.user_ref, AWORKER_IPC_BIND_FAILED) == 0);
	fw_send(&fw[0], msg);
	pump();
	CHECK(us0.available == 0);
	CHECK(!aworker_sccp_user_ready(us0.user));
	CHECK(bsc_sccp_tx_data_msg(us0.user, 1, data_msgb(4, 0)) == -ENOTCONN);
	/* And it is not retried in a loop while the worker stays up. */
	pump();
	CHECK(fw_idle(&fw[0]));
	printf("  a refused bind leaves the user unusable and is not retried on a running worker\n");

	us_free(&us0);
	fixture_down();
}

int main(void)
{
	size_t blocks;

	ctx = talloc_named_const(NULL, 0, "aworker_pool_test");
	tall_bsc_ctx = ctx;
	msgb_talloc_ctx_init(ctx, 0);
	osmo_init_logging2(ctx, &test_log_info);
	log_set_print_filename2(osmo_stderr_target, LOG_FILENAME_NONE);
	log_set_print_category(osmo_stderr_target, 0);
	log_set_print_category_hex(osmo_stderr_target, 0);
	log_set_print_level(osmo_stderr_target, 1);
	log_set_use_color(osmo_stderr_target, 0);
	/* The IPC link refuses to run unless SIGPIPE is ignored. */
	signal(SIGPIPE, SIG_IGN);
	OSMO_ASSERT(osmo_ss7_init() == 0);
	blocks = talloc_total_blocks(ctx);

	test_t1_bind_and_routing();
	test_t2_transmit();
	test_t3_limits();
	test_t4_uplink();
	test_t5_worker_lost();
	test_t6_unbind_and_stop();
	test_t7_duplicate_cs7();
	test_t8_bind_refused();

	printf("talloc blocks at the end: %s\n", talloc_total_blocks(ctx) == blocks ? "unchanged" : "grown");
	CHECK(talloc_total_blocks(ctx) == blocks);
	printf("%s\n", failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}

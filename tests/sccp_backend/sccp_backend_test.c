/* Test the in-process SCCP User backend (bsc_sccp_backend.c) against the libosmo-sigtran API it must call.
 *
 * Every libosmo-sigtran function the backend uses is replaced at link time (-Wl,--wrap=...) by a recording fake below,
 * so this test runs the real backend code and observes exactly which library call it makes, with which arguments, and
 * what happens to msgb ownership and return codes. No SCCP stack, socket or MSC is involved.
 */
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

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/prim.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/sccp_helpers.h>

#include <osmocom/bsc/bsc_sccp_backend.h>

static void *ctx;

/* Opaque stand-ins for libosmo-sigtran objects. The code under test only passes them on to (wrapped) library calls;
 * it must never dereference them. */
static int fake_sccp_storage, fake_bsc_sccp_storage, fake_scu_storage;
#define FAKE_SCCP	((struct osmo_sccp_instance *)&fake_sccp_storage)
#define FAKE_BSC_SCCP	((struct bsc_sccp_inst *)&fake_bsc_sccp_storage)
#define FAKE_SCU	((struct osmo_sccp_user *)&fake_scu_storage)

/* ---------------------------------------------------------------------------------------------------------------
 * Recording fakes of the libosmo-sigtran functions used by bsc_sccp_backend.c
 * --------------------------------------------------------------------------------------------------------------- */

static struct {
	const char *func;
	unsigned int calls;
	struct osmo_sccp_user *scu;
	uint32_t conn_id;
	uint32_t cause;
	const struct osmo_sccp_addr *calling_ptr, *called_ptr, *resp_ptr;
	struct osmo_sccp_addr calling, called, resp;
	struct msgb *msg;
	const uint8_t *data_ptr;
	uint8_t data[64];
	unsigned int len;
} rec;

/* The return code the next wrapped tx helper gives back. */
static int next_rc;

static void rec_reset(void)
{
	memset(&rec, 0, sizeof(rec));
}

static void rec_addr(const struct osmo_sccp_addr *src, const struct osmo_sccp_addr **ptr, struct osmo_sccp_addr *copy)
{
	*ptr = src;
	if (src)
		*copy = *src;
}

static void rec_data(const uint8_t *data, unsigned int len)
{
	OSMO_ASSERT(len <= sizeof(rec.data));
	rec.data_ptr = data;
	rec.len = len;
	if (data && len)
		memcpy(rec.data, data, len);
}

/* Like the real library helpers, the *_msg variants consume msg on every path, success or error
 * (libosmo-sigtran src/sccp_helpers.c: osmo_sccp_tx_unitdata_msg(), osmo_sccp_tx_conn_req_msg(),
 * osmo_sccp_tx_data_msg() always end in msgb_free(msg)). */
static void rec_consume_msg(struct msgb *msg)
{
	rec.msg = msg;
	rec_data(msgb_data(msg), msgb_length(msg));
	msgb_free(msg);
}

int __wrap_osmo_sccp_tx_unitdata_msg(struct osmo_sccp_user *scu,
				     const struct osmo_sccp_addr *calling_addr,
				     const struct osmo_sccp_addr *called_addr,
				     struct msgb *msg)
{
	rec.func = __func__; rec.calls++; rec.scu = scu;
	rec_addr(calling_addr, &rec.calling_ptr, &rec.calling);
	rec_addr(called_addr, &rec.called_ptr, &rec.called);
	rec_consume_msg(msg);
	return next_rc;
}

int __wrap_osmo_sccp_tx_conn_req_msg(struct osmo_sccp_user *scu, uint32_t conn_id,
				     const struct osmo_sccp_addr *calling_addr,
				     const struct osmo_sccp_addr *called_addr,
				     struct msgb *msg)
{
	rec.func = __func__; rec.calls++; rec.scu = scu; rec.conn_id = conn_id;
	rec_addr(calling_addr, &rec.calling_ptr, &rec.calling);
	rec_addr(called_addr, &rec.called_ptr, &rec.called);
	rec_consume_msg(msg);
	return next_rc;
}

int __wrap_osmo_sccp_tx_conn_resp(struct osmo_sccp_user *scu, uint32_t conn_id,
				  const struct osmo_sccp_addr *resp_addr,
				  const uint8_t *data, unsigned int len)
{
	rec.func = __func__; rec.calls++; rec.scu = scu; rec.conn_id = conn_id;
	rec_addr(resp_addr, &rec.resp_ptr, &rec.resp);
	rec_data(data, len);
	return next_rc;
}

int __wrap_osmo_sccp_tx_data_msg(struct osmo_sccp_user *scu, uint32_t conn_id, struct msgb *msg)
{
	rec.func = __func__; rec.calls++; rec.scu = scu; rec.conn_id = conn_id;
	rec_consume_msg(msg);
	return next_rc;
}

int __wrap_osmo_sccp_tx_disconn(struct osmo_sccp_user *scu, uint32_t conn_id,
				const struct osmo_sccp_addr *resp_addr, uint32_t cause)
{
	rec.func = __func__; rec.calls++; rec.scu = scu; rec.conn_id = conn_id; rec.cause = cause;
	rec_addr(resp_addr, &rec.resp_ptr, &rec.resp);
	return next_rc;
}

/* SCCP User management fakes */
static struct {
	struct osmo_sccp_instance *inst;
	const char *name;
	osmo_prim_cb prim_cb;
	uint16_t ssn;
	uint32_t pc;
	bool fail;
	void *priv;
	unsigned int set_priv_calls;
	unsigned int unbind_calls;
	struct osmo_sccp_user *unbound;
	struct osmo_sccp_user *find_result;
} usr;

struct osmo_sccp_user *__wrap_osmo_sccp_user_bind(struct osmo_sccp_instance *inst, const char *name,
						  osmo_prim_cb prim_cb, uint16_t ssn)
{
	usr.inst = inst; usr.name = name; usr.prim_cb = prim_cb; usr.ssn = ssn;
	return usr.fail ? NULL : FAKE_SCU;
}

struct osmo_sccp_user *__wrap_osmo_sccp_user_find(struct osmo_sccp_instance *inst, uint16_t ssn, uint32_t pc)
{
	usr.inst = inst; usr.ssn = ssn; usr.pc = pc;
	return usr.find_result;
}

void __wrap_osmo_sccp_user_unbind(struct osmo_sccp_user *scu)
{
	usr.unbind_calls++;
	usr.unbound = scu;
}

void __wrap_osmo_sccp_user_set_priv(struct osmo_sccp_user *scu, void *priv)
{
	OSMO_ASSERT(scu == FAKE_SCU);
	usr.set_priv_calls++;
	usr.priv = priv;
}

void *__wrap_osmo_sccp_user_get_priv(struct osmo_sccp_user *scu)
{
	OSMO_ASSERT(scu == FAKE_SCU);
	return usr.priv;
}

struct osmo_sccp_instance *__wrap_osmo_sccp_get_sccp(const struct osmo_sccp_user *scu)
{
	OSMO_ASSERT(scu == FAKE_SCU);
	return FAKE_SCCP;
}

void *__wrap_osmo_sccp_get_priv(struct osmo_sccp_instance *sccp)
{
	OSMO_ASSERT(sccp == FAKE_SCCP);
	return FAKE_BSC_SCCP;
}

/* Compile-time check that each fake has exactly the prototype of the library function it replaces. */
static __typeof__(osmo_sccp_tx_unitdata_msg) *const chk_unitdata = __wrap_osmo_sccp_tx_unitdata_msg;
static __typeof__(osmo_sccp_tx_conn_req_msg) *const chk_conn_req = __wrap_osmo_sccp_tx_conn_req_msg;
static __typeof__(osmo_sccp_tx_conn_resp) *const chk_conn_resp = __wrap_osmo_sccp_tx_conn_resp;
static __typeof__(osmo_sccp_tx_data_msg) *const chk_data = __wrap_osmo_sccp_tx_data_msg;
static __typeof__(osmo_sccp_tx_disconn) *const chk_disconn = __wrap_osmo_sccp_tx_disconn;
static __typeof__(osmo_sccp_user_bind) *const chk_bind = __wrap_osmo_sccp_user_bind;
static __typeof__(osmo_sccp_user_find) *const chk_find = __wrap_osmo_sccp_user_find;
static __typeof__(osmo_sccp_user_unbind) *const chk_unbind = __wrap_osmo_sccp_user_unbind;
static __typeof__(osmo_sccp_user_set_priv) *const chk_set_priv = __wrap_osmo_sccp_user_set_priv;
static __typeof__(osmo_sccp_user_get_priv) *const chk_get_priv = __wrap_osmo_sccp_user_get_priv;
static __typeof__(osmo_sccp_get_sccp) *const chk_get_sccp = __wrap_osmo_sccp_get_sccp;
static __typeof__(osmo_sccp_get_priv) *const chk_get_bsc_sccp = __wrap_osmo_sccp_get_priv;

/* ---------------------------------------------------------------------------------------------------------------
 * Test helpers
 * --------------------------------------------------------------------------------------------------------------- */

/* Addresses with every field of struct osmo_sccp_addr set to a distinct value, so that a field that got lost, zeroed
 * or swapped shows up in the full-struct comparison. */
static struct osmo_sccp_addr addr_a, addr_b;

static void init_addrs(void)
{
	memset(&addr_a, 0, sizeof(addr_a));
	addr_a.presence = OSMO_SCCP_ADDR_T_GT | OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN | OSMO_SCCP_ADDR_T_IPv4;
	addr_a.ri = OSMO_SCCP_RI_SSN_PC;
	addr_a.gt.gti = OSMO_SCCP_GTI_TT_NPL_ENC_NAI;
	addr_a.gt.tt = 7;
	addr_a.gt.npi = 1;
	addr_a.gt.nai = 4;
	OSMO_STRLCPY_ARRAY(addr_a.gt.digits, "4912345678");
	addr_a.pc = 0x0123;
	addr_a.ssn = OSMO_SCCP_SSN_BSSAP;
	addr_a.ip.v4.s_addr = htonl(0x7f000001);

	memset(&addr_b, 0, sizeof(addr_b));
	addr_b.presence = OSMO_SCCP_ADDR_T_PC | OSMO_SCCP_ADDR_T_SSN | OSMO_SCCP_ADDR_T_IPv6;
	addr_b.ri = OSMO_SCCP_RI_GT;
	addr_b.gt.gti = OSMO_SCCP_GTI_NAI_ONLY;
	addr_b.gt.nai = 3;
	OSMO_STRLCPY_ARRAY(addr_b.gt.digits, "123");
	addr_b.pc = 0x3fff;
	addr_b.ssn = OSMO_SCCP_SSN_SMLC_BSSAP_LE;
	addr_b.ip.v6.s6_addr[15] = 1;
}

/* Compare the address the library fake received against what the caller passed: same pointer (passed through
 * unchanged) and same contents, every byte of the struct. */
static void check_addr(const char *label, const struct osmo_sccp_addr *expect,
		       const struct osmo_sccp_addr *got_ptr, const struct osmo_sccp_addr *got_copy)
{
	if (!expect) {
		OSMO_ASSERT(got_ptr == NULL);
		printf("  %s: NULL, passed through\n", label);
		return;
	}
	OSMO_ASSERT(got_ptr == expect);
	OSMO_ASSERT(memcmp(got_copy, expect, sizeof(*expect)) == 0);
	printf("  %s: same pointer, all %zu bytes equal (presence=0x%x pc=0x%x ssn=%u gt='%s')\n",
	       label, sizeof(*expect), got_copy->presence, got_copy->pc, got_copy->ssn, got_copy->gt.digits);
}

static const uint8_t payload[] = { 0x00, 0x04, 0x30, 0x04, 0x01, 0x20 };

static struct msgb *payload_msg(void)
{
	struct msgb *msg = msgb_alloc_headroom(1024, 128, "test payload");
	OSMO_ASSERT(msg);
	memcpy(msgb_put(msg, sizeof(payload)), payload, sizeof(payload));
	return msg;
}

static void check_payload(void)
{
	OSMO_ASSERT(rec.len == sizeof(payload));
	OSMO_ASSERT(memcmp(rec.data, payload, sizeof(payload)) == 0);
	printf("  payload: %u bytes, equal\n", rec.len);
}

/* All msgbs and bsc_sccp_users are allocated below ctx; a leak or a missing free changes this count, a double free
 * aborts inside talloc. */
static size_t blocks(void)
{
	return talloc_total_blocks(ctx);
}

/* A bsc_sccp_user bound through the real bsc_sccp_user_bind() on the fake library. */
static struct bsc_sccp_user *test_user;

static const int rcs[] = { 0, -ENOTCONN, -EMSGSIZE, -EINVAL };

/* ---------------------------------------------------------------------------------------------------------------
 * Tests: down direction (osmo-bsc -> SCCP stack)
 * --------------------------------------------------------------------------------------------------------------- */

static void test_tx_unitdata_msg(void)
{
	int i;
	printf("\n%s\n", __func__);
	for (i = 0; i < ARRAY_SIZE(rcs); i++) {
		size_t before = blocks();
		struct msgb *msg = payload_msg();
		int rc;

		rec_reset();
		next_rc = rcs[i];
		rc = bsc_sccp_tx_unitdata_msg(test_user, &addr_a, &addr_b, msg);
		printf(" library rc=%d -> rc=%d; %s called %u time(s)\n", next_rc, rc, rec.func, rec.calls);
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && !strcmp(rec.func, "__wrap_osmo_sccp_tx_unitdata_msg"));
		OSMO_ASSERT(rec.scu == FAKE_SCU);
		OSMO_ASSERT(rec.msg == msg);
		check_addr("calling_addr", &addr_a, rec.calling_ptr, &rec.calling);
		check_addr("called_addr", &addr_b, rec.called_ptr, &rec.called);
		check_payload();
		OSMO_ASSERT(blocks() == before);
		printf("  msgb: consumed exactly once\n");
	}
}

static void test_tx_conn_req_msg(void)
{
	int i;
	printf("\n%s\n", __func__);
	for (i = 0; i < ARRAY_SIZE(rcs); i++) {
		size_t before = blocks();
		struct msgb *msg = payload_msg();
		int rc;

		rec_reset();
		next_rc = rcs[i];
		rc = bsc_sccp_tx_conn_req_msg(test_user, 0x00fffffe, &addr_a, &addr_b, msg);
		printf(" library rc=%d -> rc=%d; %s called %u time(s)\n", next_rc, rc, rec.func, rec.calls);
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && !strcmp(rec.func, "__wrap_osmo_sccp_tx_conn_req_msg"));
		OSMO_ASSERT(rec.scu == FAKE_SCU);
		OSMO_ASSERT(rec.conn_id == 0x00fffffe);
		printf("  conn_id: 0x%x\n", rec.conn_id);
		OSMO_ASSERT(rec.msg == msg);
		check_addr("calling_addr", &addr_a, rec.calling_ptr, &rec.calling);
		check_addr("called_addr", &addr_b, rec.called_ptr, &rec.called);
		check_payload();
		OSMO_ASSERT(blocks() == before);
		printf("  msgb: consumed exactly once\n");
	}
}

static void test_tx_conn_resp(void)
{
	int i;
	printf("\n%s\n", __func__);
	for (i = 0; i < ARRAY_SIZE(rcs); i++) {
		size_t before = blocks();
		int rc;

		/* As called by osmo-bsc (bsc_subscr_conn_fsm.c): without data */
		rec_reset();
		next_rc = rcs[i];
		rc = bsc_sccp_tx_conn_resp(test_user, 42, &addr_b, NULL, 0);
		printf(" library rc=%d -> rc=%d; %s called %u time(s)\n", next_rc, rc, rec.func, rec.calls);
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && !strcmp(rec.func, "__wrap_osmo_sccp_tx_conn_resp"));
		OSMO_ASSERT(rec.scu == FAKE_SCU);
		OSMO_ASSERT(rec.conn_id == 42);
		check_addr("resp_addr", &addr_b, rec.resp_ptr, &rec.resp);
		OSMO_ASSERT(rec.data_ptr == NULL && rec.len == 0);
		printf("  data: NULL, len 0, passed through\n");

		/* With data: the same buffer pointer must reach the library */
		rec_reset();
		rc = bsc_sccp_tx_conn_resp(test_user, 43, &addr_a, payload, sizeof(payload));
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && rec.conn_id == 43);
		check_addr("resp_addr", &addr_a, rec.resp_ptr, &rec.resp);
		OSMO_ASSERT(rec.data_ptr == payload);
		check_payload();
		OSMO_ASSERT(blocks() == before);
		printf("  no allocation left behind\n");
	}
}

static void test_tx_data_msg(void)
{
	int i;
	printf("\n%s\n", __func__);
	for (i = 0; i < ARRAY_SIZE(rcs); i++) {
		size_t before = blocks();
		struct msgb *msg = payload_msg();
		int rc;

		rec_reset();
		next_rc = rcs[i];
		rc = bsc_sccp_tx_data_msg(test_user, 0, msg);
		printf(" library rc=%d -> rc=%d; %s called %u time(s)\n", next_rc, rc, rec.func, rec.calls);
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && !strcmp(rec.func, "__wrap_osmo_sccp_tx_data_msg"));
		OSMO_ASSERT(rec.scu == FAKE_SCU);
		OSMO_ASSERT(rec.conn_id == 0);
		printf("  conn_id: 0x%x\n", rec.conn_id);
		OSMO_ASSERT(rec.msg == msg);
		check_payload();
		OSMO_ASSERT(blocks() == before);
		printf("  msgb: consumed exactly once\n");
	}
}

static void test_tx_disconn(void)
{
	int i;
	printf("\n%s\n", __func__);
	for (i = 0; i < ARRAY_SIZE(rcs); i++) {
		size_t before = blocks();
		int rc;

		rec_reset();
		next_rc = rcs[i];
		rc = bsc_sccp_tx_disconn(test_user, 0x123456, &addr_a, 0x5a);
		printf(" library rc=%d -> rc=%d; %s called %u time(s)\n", next_rc, rc, rec.func, rec.calls);
		OSMO_ASSERT(rc == next_rc);
		OSMO_ASSERT(rec.calls == 1 && !strcmp(rec.func, "__wrap_osmo_sccp_tx_disconn"));
		OSMO_ASSERT(rec.scu == FAKE_SCU);
		OSMO_ASSERT(rec.conn_id == 0x123456 && rec.cause == 0x5a);
		printf("  conn_id: 0x%x cause: 0x%x\n", rec.conn_id, rec.cause);
		check_addr("resp_addr", &addr_a, rec.resp_ptr, &rec.resp);

		/* NULL resp_addr must stay NULL */
		rec_reset();
		rc = bsc_sccp_tx_disconn(test_user, 1, NULL, 0);
		OSMO_ASSERT(rc == next_rc && rec.calls == 1);
		check_addr("resp_addr", NULL, rec.resp_ptr, &rec.resp);
		OSMO_ASSERT(blocks() == before);
	}
}

/* ---------------------------------------------------------------------------------------------------------------
 * Tests: user management and the up direction (SCCP stack -> osmo-bsc)
 * --------------------------------------------------------------------------------------------------------------- */

static struct {
	unsigned int calls;
	struct bsc_sccp_user *user;
	struct osmo_prim_hdr *oph;
} upcall;

static int test_prim_cb(struct bsc_sccp_user *user, struct osmo_prim_hdr *oph)
{
	upcall.calls++;
	upcall.user = user;
	upcall.oph = oph;
	/* Same contract as osmo-bsc's sccp_sap_up(): the callee owns and frees the msgb */
	msgb_free(oph->msg);
	return 42;
}

static void test_bind(void)
{
	size_t before;

	printf("\n%s\n", __func__);
	memset(&usr, 0, sizeof(usr));
	before = blocks();
	test_user = bsc_sccp_user_bind(ctx, FAKE_SCCP, "msc-0", test_prim_cb, OSMO_SCCP_SSN_BSSAP);
	OSMO_ASSERT(test_user);
	OSMO_ASSERT(usr.inst == FAKE_SCCP && usr.ssn == OSMO_SCCP_SSN_BSSAP && !strcmp(usr.name, "msc-0"));
	printf(" osmo_sccp_user_bind(inst, name='%s', ssn=%u)\n", usr.name, usr.ssn);
	OSMO_ASSERT(usr.prim_cb != NULL);
	OSMO_ASSERT(test_user->scu == FAKE_SCU);
	OSMO_ASSERT(test_user->ops == &bsc_sccp_backend_local_ops);
	OSMO_ASSERT(test_user->prim_cb == test_prim_cb);
	OSMO_ASSERT(usr.set_priv_calls == 1 && usr.priv == test_user);
	printf(" backend '%s', wraps the bound SCCP user, registered as its priv\n", test_user->ops->name);
	OSMO_ASSERT(blocks() == before + 1);
	printf(" one allocation (the bsc_sccp_user) below the given ctx\n");

	OSMO_ASSERT(bsc_sccp_user_get_sccp(test_user) == FAKE_SCCP);
	OSMO_ASSERT(bsc_sccp_user_get_bsc_sccp(test_user) == FAKE_BSC_SCCP);
	printf(" get_sccp() and get_bsc_sccp() resolve through the SCCP user and instance\n");
}

static void test_bind_fails(void)
{
	size_t before = blocks();
	struct bsc_sccp_user *u;

	printf("\n%s\n", __func__);
	usr.fail = true;
	usr.set_priv_calls = 0;
	u = bsc_sccp_user_bind(ctx, FAKE_SCCP, "msc-1", test_prim_cb, OSMO_SCCP_SSN_BSSAP);
	usr.fail = false;
	OSMO_ASSERT(u == NULL);
	OSMO_ASSERT(usr.set_priv_calls == 0);
	OSMO_ASSERT(blocks() == before);
	printf(" osmo_sccp_user_bind() failure -> NULL, nothing allocated, priv not touched\n");
	/* restore the priv of the user bound in test_bind() */
	usr.priv = test_user;
}

static void test_find(void)
{
	printf("\n%s\n", __func__);
	usr.find_result = FAKE_SCU;
	OSMO_ASSERT(bsc_sccp_user_find(FAKE_SCCP, OSMO_SCCP_SSN_BSSAP, 0x0123) == test_user);
	OSMO_ASSERT(usr.inst == FAKE_SCCP && usr.ssn == OSMO_SCCP_SSN_BSSAP && usr.pc == 0x0123);
	printf(" bound user found: returns the same bsc_sccp_user (ssn=%u pc=0x%x passed through)\n", usr.ssn, usr.pc);

	usr.find_result = NULL;
	OSMO_ASSERT(bsc_sccp_user_find(FAKE_SCCP, 250, 0x0042) == NULL);
	OSMO_ASSERT(usr.ssn == 250 && usr.pc == 0x0042);
	printf(" no user: returns NULL\n");
}

static void test_upcall(void)
{
	size_t before = blocks();
	struct msgb *msg;
	struct osmo_scu_prim *prim;
	int rc;

	printf("\n%s\n", __func__);
	msg = msgb_alloc_headroom(1024, 128, "test prim");
	prim = (struct osmo_scu_prim *)msgb_put(msg, sizeof(*prim));
	osmo_prim_init(&prim->oph, SCCP_SAP_USER, OSMO_SCU_PRIM_N_DATA, PRIM_OP_INDICATION, msg);
	prim->u.data.conn_id = 7;

	memset(&upcall, 0, sizeof(upcall));
	/* What libosmo-sigtran does: sccp_user_prim_up() -> scu->prim_cb(&prim->oph, scu) */
	rc = usr.prim_cb(&prim->oph, FAKE_SCU);
	OSMO_ASSERT(rc == 42);
	OSMO_ASSERT(upcall.calls == 1 && upcall.user == test_user && upcall.oph == &prim->oph);
	printf(" N-DATA.ind delivered once to the bsc_sccp_user's callback, same primitive, rc=%d returned\n", rc);
	OSMO_ASSERT(blocks() == before);
	printf(" msgb ownership passed to the callback (freed there, exactly once)\n");
}

static void test_unbind(void)
{
	size_t before = blocks();

	printf("\n%s\n", __func__);
	bsc_sccp_user_unbind(test_user);
	OSMO_ASSERT(usr.unbind_calls == 1 && usr.unbound == FAKE_SCU);
	OSMO_ASSERT(blocks() == before - 1);
	printf(" osmo_sccp_user_unbind() called once on the wrapped user; bsc_sccp_user freed\n");
	test_user = NULL;

	bsc_sccp_user_unbind(NULL);
	OSMO_ASSERT(usr.unbind_calls == 1);
	printf(" unbind(NULL) is a no-op\n");
}

int main(int argc, char **argv)
{
	ctx = talloc_named_const(NULL, 0, "sccp_backend_test");
	msgb_talloc_ctx_init(ctx, 0);
	init_addrs();

	/* reference the prototype checks so that they are not optimised away as unused */
	OSMO_ASSERT(chk_unitdata && chk_conn_req && chk_conn_resp && chk_data && chk_disconn && chk_bind && chk_find
		    && chk_unbind && chk_set_priv && chk_get_priv && chk_get_sccp && chk_get_bsc_sccp);

	test_bind();
	test_bind_fails();
	test_find();
	test_tx_unitdata_msg();
	test_tx_conn_req_msg();
	test_tx_conn_resp();
	test_tx_data_msg();
	test_tx_disconn();
	test_upcall();
	test_unbind();

	printf("\ndone\n");
	/* Release the root context, so that a run under LeakSanitizer reports nothing at exit. */
	talloc_free(ctx);
	return 0;
}

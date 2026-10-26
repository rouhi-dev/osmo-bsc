AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	-ggdb3 \
	$(LIBOSMOCORE_CFLAGS) \
	$(LIBOSMOSIGTRAN_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

# Replace every libosmo-sigtran function used by bsc_sccp_backend.c with a recording fake from the test.
AM_LDFLAGS = \
	$(COVERAGE_LDFLAGS) \
	-no-install \
	-Wl,--wrap=osmo_sccp_tx_unitdata_msg \
	-Wl,--wrap=osmo_sccp_tx_conn_req_msg \
	-Wl,--wrap=osmo_sccp_tx_conn_resp \
	-Wl,--wrap=osmo_sccp_tx_data_msg \
	-Wl,--wrap=osmo_sccp_tx_disconn \
	-Wl,--wrap=osmo_sccp_user_bind \
	-Wl,--wrap=osmo_sccp_user_find \
	-Wl,--wrap=osmo_sccp_user_unbind \
	-Wl,--wrap=osmo_sccp_user_set_priv \
	-Wl,--wrap=osmo_sccp_user_get_priv \
	-Wl,--wrap=osmo_sccp_get_sccp \
	-Wl,--wrap=osmo_sccp_get_priv \
	$(NULL)

EXTRA_DIST = \
	sccp_backend_test.ok \
	$(NULL)

check_PROGRAMS = \
	sccp_backend_test \
	$(NULL)

sccp_backend_test_SOURCES = \
	sccp_backend_test.c \
	$(NULL)

sccp_backend_test_LDADD = \
	$(top_builddir)/src/osmo-bsc/libbsc.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/sccp_backend_test > $(srcdir)/sccp_backend_test.ok

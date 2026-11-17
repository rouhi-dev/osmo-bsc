AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	-ggdb3 \
	$(LIBOSMOCORE_CFLAGS) \
	$(LIBOSMOGSM_CFLAGS) \
	$(LIBOSMOVTY_CFLAGS) \
	$(LIBOSMOSIGTRAN_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(COVERAGE_LDFLAGS) \
	-no-install \
	$(NULL)

EXTRA_DIST = \
	aworker_worker_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_worker_test \
	$(NULL)

aworker_worker_test_SOURCES = \
	aworker_worker_test.c \
	$(NULL)

# What the worker in the child process does is recorded through -Wl,--wrap (see the test): whether it parses its
# configuration at all, and the view check's result (one case bypasses it). Unarmed, the wrappers only record.
aworker_worker_test_LDFLAGS = \
	$(AM_LDFLAGS) \
	-Wl,--wrap=vty_read_config_filep \
	-Wl,--wrap=aworker_cfg_check_worker_view \
	$(NULL)

# Only the A-worker library (and the Osmocom libraries): the worker role does not depend on osmo-bsc.
aworker_worker_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_worker_test $(top_builddir)/src/osmo-bsc/osmo-bsc > $(srcdir)/aworker_worker_test.ok

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
	aworker_supervisor_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_supervisor_test \
	$(NULL)

aworker_supervisor_test_SOURCES = \
	aworker_supervisor_test.c \
	$(NULL)

# Only the A-worker library (and the Osmocom libraries): the Supervisor does not depend on osmo-bsc.
aworker_supervisor_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_supervisor_test > $(srcdir)/aworker_supervisor_test.ok

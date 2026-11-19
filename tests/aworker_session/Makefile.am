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
	aworker_wsplit_test.ok \
	aworker_session_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_wsplit_test \
	aworker_session_test \
	$(NULL)

aworker_wsplit_test_SOURCES = \
	aworker_wsplit_test.c \
	$(NULL)

aworker_session_test_SOURCES = \
	aworker_session_test.c \
	$(NULL)

aworker_session_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

aworker_wsplit_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_session_test $(top_builddir)/src/a-worker/bsc-aworker > $(srcdir)/aworker_session_test.ok
	$(builddir)/aworker_wsplit_test $(top_builddir)/src/a-worker/bsc-aworker $(top_builddir)/src/osmo-bsc/osmo-bsc > $(srcdir)/aworker_wsplit_test.ok

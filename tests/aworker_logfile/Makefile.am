AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	-ggdb3 \
	$(LIBOSMOCORE_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(COVERAGE_LDFLAGS) \
	-no-install \
	$(NULL)

EXTRA_DIST = \
	aworker_logfile_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_logfile_test \
	$(NULL)

# D-S10 (decision B2): libosmocore's file target only; nothing of osmo-bsc or the A-worker code.
aworker_logfile_test_SOURCES = \
	aworker_logfile_test.c \
	$(NULL)

aworker_logfile_test_LDADD = \
	$(LIBOSMOCORE_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_logfile_test > $(srcdir)/aworker_logfile_test.ok

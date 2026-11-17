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
	aworker_cfg_view_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_cfg_view_test \
	$(NULL)

aworker_cfg_view_test_SOURCES = \
	aworker_cfg_view_test.c \
	$(NULL)

# Only the A-worker library and the Osmocom libraries: the views do not depend on osmo-bsc.
# Part D4 replaces fstat() and pread() for the calls the A-worker library (linked statically from libaworker.a) and
# this test make: see __wrap_fstat() and __wrap_pread() in the test. Unless a D4 case arms them, they only pass the
# call on. Nothing of this reaches osmo-bsc, which is linked without these options.
aworker_cfg_view_test_LDFLAGS = \
	$(AM_LDFLAGS) \
	-Wl,--wrap=fstat \
	-Wl,--wrap=pread \
	$(NULL)

aworker_cfg_view_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_cfg_view_test > $(srcdir)/aworker_cfg_view_test.ok

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
	aworker_proc_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_proc_test \
	$(NULL)

aworker_proc_test_SOURCES = \
	aworker_proc_test.c \
	$(NULL)

# Link-time seams of the test (see the comment at the top of aworker_proc_test.c). Per-target LDFLAGS replace
# AM_LDFLAGS, so repeat them.
aworker_proc_test_LDFLAGS = \
	$(AM_LDFLAGS) \
	-Wl,--wrap=waitpid \
	-Wl,--wrap=prctl \
	-Wl,--wrap=fork \
	$(NULL)

aworker_proc_test_LDADD = \
	$(top_builddir)/src/osmo-bsc/libbsc.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_proc_test > $(srcdir)/aworker_proc_test.ok

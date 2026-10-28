AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	-ggdb3 \
	$(LIBOSMOCORE_CFLAGS) \
	$(LIBOSMOGSM_CFLAGS) \
	$(LIBOSMOSIGTRAN_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(COVERAGE_LDFLAGS) \
	-no-install \
	$(NULL)

EXTRA_DIST = \
	aworker_ipc_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_ipc_test \
	$(NULL)

aworker_ipc_test_SOURCES = \
	aworker_ipc_test.c \
	$(NULL)

# -ldl: dlsym(RTLD_NEXT) of the sendmsg() fault injection (a separate library before glibc 2.34)
aworker_ipc_test_LDADD = \
	$(top_builddir)/src/osmo-bsc/libbsc.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	-ldl \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_ipc_test > $(srcdir)/aworker_ipc_test.ok

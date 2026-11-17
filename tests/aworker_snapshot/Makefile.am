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
	aworker_snapshot_test.ok \
	$(NULL)

check_PROGRAMS = \
	aworker_snapshot_test \
	aworker_snapshot_syscall_test \
	$(NULL)

# The snapshot module as libaworker has it (memfd_create() of the C library when configure found it). Failures of
# memfd_create(), fcntl(), fstat(), pread(), open(), openat() and mkdirat() are injected through -Wl,--wrap (see
# the test): the module has no test code of its own.
aworker_snapshot_test_SOURCES = \
	aworker_snapshot_test.c \
	$(NULL)

aworker_snapshot_test_LDFLAGS = \
	$(AM_LDFLAGS) \
	-Wl,--wrap=memfd_create \
	-Wl,--wrap=fcntl \
	-Wl,--wrap=fstat \
	-Wl,--wrap=pread \
	-Wl,--wrap=__pread_chk \
	-Wl,--wrap=open \
	-Wl,--wrap=__open_2 \
	-Wl,--wrap=openat \
	-Wl,--wrap=__openat_2 \
	-Wl,--wrap=mkdirat \
	$(NULL)

# Only the A-worker library (and what it needs): no osmo-bsc code.
aworker_snapshot_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(NULL)

# The same tests against the module compiled with its system-call path (AWORKER_SNAPSHOT_USE_SYSCALL), so that the
# path a C library without memfd_create() takes is compiled and tested too. aworker_snapshot_syscall.c includes the
# module's source; the program's own copy of it is linked before libaworker.la.
aworker_snapshot_syscall_test_SOURCES = \
	aworker_snapshot_test.c \
	aworker_snapshot_syscall.c \
	$(NULL)

aworker_snapshot_syscall_test_CPPFLAGS = \
	$(AM_CPPFLAGS) \
	-I$(top_srcdir)/src/a-worker \
	-DAWORKER_SNAPSHOT_USE_SYSCALL \
	$(NULL)

aworker_snapshot_syscall_test_LDFLAGS = \
	$(AM_LDFLAGS) \
	-Wl,--wrap=syscall \
	-Wl,--wrap=fcntl \
	-Wl,--wrap=fstat \
	-Wl,--wrap=pread \
	-Wl,--wrap=__pread_chk \
	-Wl,--wrap=open \
	-Wl,--wrap=__open_2 \
	-Wl,--wrap=openat \
	-Wl,--wrap=__openat_2 \
	-Wl,--wrap=mkdirat \
	$(NULL)

aworker_snapshot_syscall_test_LDADD = \
	$(top_builddir)/src/a-worker/libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(NULL)

.PHONY: update_exp
update_exp:
	$(builddir)/aworker_snapshot_test > $(srcdir)/aworker_snapshot_test.ok

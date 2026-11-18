AM_CPPFLAGS = \
	$(all_includes) \
	-I$(top_srcdir)/include \
	-I$(top_builddir) \
	$(NULL)

AM_CFLAGS = \
	-Wall \
	$(LIBOSMOCORE_CFLAGS) \
	$(LIBOSMOGSM_CFLAGS) \
	$(LIBOSMOVTY_CFLAGS) \
	$(LIBOSMOSIGTRAN_CFLAGS) \
	$(COVERAGE_CFLAGS) \
	$(NULL)

AM_LDFLAGS = \
	$(COVERAGE_LDFLAGS) \
	$(NULL)

# The A-worker code that does not depend on osmo-bsc: the worker process role, the IPC between worker and its
# parent, the process life cycle of the pool, the 'a-worker-pool' configuration and the shared log categories.
# osmo-bsc links it through libbsc.la; see docs/aworker/PHASE2_SUPERVISOR.md section 14.
noinst_LTLIBRARIES = libaworker.la

libaworker_la_SOURCES = \
	aworker.c \
	aworker_canon.c \
	aworker_cfg_scan.c \
	aworker_cfg_view.c \
	aworker_ipc.c \
	aworker_ipc_link.c \
	aworker_log.c \
	aworker_pool.c \
	aworker_snapshot.c \
	aworker_vty.c \
	$(NULL)

libaworker_la_LIBADD = \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

# The A-worker Supervisor (S2-3): a program of its own, linked against libaworker and the Osmocom libraries only --
# nothing of osmo-bsc. It also runs its workers, by re-executing itself (docs/aworker/PHASE2_SUPERVISOR.md).
bin_PROGRAMS = bsc-aworker

bsc_aworker_SOURCES = \
	aworker_main.c \
	$(NULL)

bsc_aworker_LDADD = \
	libaworker.la \
	$(LIBOSMOCORE_LIBS) \
	$(LIBOSMOGSM_LIBS) \
	$(LIBOSMOVTY_LIBS) \
	$(LIBOSMOSIGTRAN_LIBS) \
	$(NULL)

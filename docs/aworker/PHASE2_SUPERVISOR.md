# A-worker pool: standalone Supervisor `bsc-aworker` (BSC crash survival)

**Status: current target architecture, revision 3 (decision resolution).** This document supersedes revisions 1
and 2 of itself. Where it conflicts with `PHASE2_ARCHITECTURE.md`, this document wins (that document's revision note
says so). The phased plan is `PHASE2_IMPLEMENTATION_PLAN.md` §13 and §14.

**Implementation status (2026-11-18, after S2-3).** Implemented: S1 (source separation into `libaworker.la`), S2-1
(inline form, view builder, BSC view, refusal of every configuration write in inline mode), S2-2 (single-read
configuration, worker snapshots in **legacy mode**; accepted) and **S2-3: the `bsc-aworker` executable**, a Supervisor
that reads `osmo-bsc.cfg`, runs the pool by itself (snapshots, workers re-executing its image, restart policy, GOODBYE
on SIGTERM) and is controlled by signals only. **S2-4: IPC protocol 2.0** -- versions and identity in HELLO, message
classes per channel, descriptor passing (SCM_RIGHTS) with ATTACH/DETACH defined (§8, as implemented). **S2-5:**
with `supervisor-socket`, the Supervisor's session socket (N5), BSC sessions (§9: admission with the canonical
document, ATTACH/DETACH of per-session data links, session ECHO, fencing) and the operator channel with its client
`bsc-aworker --control`; its workers have a control link and data links (minimal detach). **Not implemented:** the
detached worker's connection release (S2-6), the BSC session client and supervisor mode in osmo-bsc (S2-7): **osmo-bsc
does not attach to `bsc-aworker` yet** (the tests script the BSC side). Where this document describes more than that,
it describes the target, not existing code. What exists is marked **as implemented** below; plan §12 and §14.5–§14.8
have the record and the verification results.

**Do not run `bsc-aworker` and osmo-bsc with the same inline pool at the same time (until S2-7).** osmo-bsc does not
attach to the Supervisor yet: with an inline pool it starts its own workers (legacy mode), so both programs would
start a worker for every cs7 instance and open the same SCTP associations twice.

Names (final, decisions B3 and 1 of 2026-11-17): the executable is **`bsc-aworker`**; snapshot memfds are named
`bsc-aworker-<index>`; fallback directories `bsc-aworker-snap.<random>`. The old name `osmo-bsc-aworker` is not
used.

**S2-0 review (recorded):** the decisions of the S2-0 review are recorded in `PHASE2_IMPLEMENTATION_PLAN.md` §14.2 and
in §16 below. The implementation subphases S2-1 … S2-8 and their mapping to the plan phases S2–S7 are in plan §14.1.
S1 is closed and accepted.

Baseline inspected for revision 3 (the implementation status above is newer):

| Component | Version / commit |
|---|---|
| osmo-bsc worktree `~/dev/osmo-bsc-a-worker-pool`, branch `a-worker-pool` | HEAD `fa01448ea` (Phase 1 + 2a-1…2a-5); 1.15.0 pin `1a2627f61` is an ancestor |
| libosmocore / libosmovty / libosmogsm / libosmoctrl | 1.14.2 (`~/dev/gsm/libs/libosmocore`) |
| libosmo-sigtran (contains SCCP) | 2.3.0 (`~/dev/gsm/libs/libosmo-sigtran`) |
| osmo-msc (peer behaviour) | 1.16.0 (`~/dev/gsm/apps/osmo-msc`) |

Evidence markers:
* **[V]** verified in the named source (file and function);
* **[A]** architecture of this document;
* **[T]** needs a test before it may be relied on.

Decision status labels (used in §16 and wherever a decision appears):
* **APPROVED DIRECTION**: given by the project owner;
* **VERIFIED**: technically verified in source;
* **PROPOSED**: awaiting approval;
* **UNRESOLVED/BLOCKING**: cannot be decided yet; the gating test or decision is named.

---

## 1. Requirement and scope (APPROVED DIRECTION)

When `osmo-bsc` crashes, including SIGKILL, the Supervisor `bsc-aworker` and its workers survive with
unchanged:

* worker PIDs;
* SCTP associations;
* M3UA ASP/AS state;
* worker-owned cs7 resources.

A BSC restarted with the same configuration reconnects to the Supervisor and attaches to the **existing** workers
without restarting them.

**Transport continuity is in scope.** **Subscriber and session continuity is not:**
* subscriber and SCCP connections are released (§10);
* the restarted BSC runs BSSMAP RESET (§11);
* no BSC state is replicated (D5).

**Supervisor crash continuity is out of scope** (§13). There is no Supervisor state replication and no second-level
Supervisor.

**No systemd or DevOps work** is part of the A-worker team's scope (§15).

## 2. Why today's code cannot survive a BSC crash [V]

| Mechanism | Source | Effect when the BSC dies |
|---|---|---|
| `prctl(PR_SET_PDEATHSIG, SIGTERM)` + `getppid()` check | `src/osmo-bsc/aworker_pool.c` `child_exec_worker()` | kernel SIGTERMs every worker |
| Link EOF ends the worker | `src/osmo-bsc/aworker.c` `w_closed_cb()` → `osmo_select_shutdown_request()` | any link close terminates the worker |
| GOODBYE at exit | `aworker_pool_stop()`; `atexit(aworker_pool_stop)` in `aworker_pool_start()` | normal BSC exit stops workers |
| socketpair IPC | `worker_spawn()` | no address to reconnect to |
| Single-use link protocol | `include/osmocom/bsc/aworker_ipc_link.h`; `AWORKER_IPC_VERSION 1` exact match | no sessions, no resume |

P2/P3 (Phase 1) deliberately assert that no worker survives its parent. They keep that meaning: the parent becomes
the Supervisor (§14).

## 3. Process roles and hierarchy [A] (APPROVED DIRECTION)

```
                       started independently of each other (operator / DevOps tooling)
   ┌──────────────────────────────┐                       ┌───────────────────────────────────────┐
   │ osmo-bsc                     │  session channel      │ bsc-aworker  (Supervisor)        │
   │  BSSAP/BSSMAP, subscribers,  │◄─────────────────────►│  worker lifecycle, ownership,         │
   │  SCCP users, conn_id registry│  AF_UNIX SEQPACKET    │  sessions, fencing, fd passing,       │
   │  MSC selection, RESET        │  (+ SCM_RIGHTS)       │  status                               │
   └──────────────┬───────────────┘                       └──────┬─────────────┬──────────────────┘
                  │ data channel (one fresh socketpair            │ control     │ control   (lifetime =
                  │ per worker per BSC session)                   │ channel     │ channel    worker process)
                  │                                    fork + fexecve(own image) ▼             ▼
                  └───────────────────────────────────────────► worker 0       worker 1   … worker N-1
                                                              (cs7 instance X: SCTP, M3UA ASP/AS,
                                                               SS7 routing, PC reachability, SCCP)
```

* **`osmo-bsc`:**
  * BSSAP/BSSMAP and subscriber state;
  * SCCP user side and the conn_id registry (`bsc_sccp.c` `bsc_sccp_inst_next_conn_id()` [V]);
  * MSC selection and admission;
  * RESET/RESET-ACK (`bssmap_reset.c`, `a_reset.c` [V]);
  * session client and data plane.

  In supervisor mode it **never** spawns, signals or restarts workers.
* **`bsc-aworker` (Supervisor):**
  * process lifecycle and reaping;
  * restart, backoff, restart limit, Policy B, FAILED, reset;
  * startup timeout and readiness tracking;
  * ownership map and incarnations;
  * control channels;
  * session ownership and fencing;
  * data-channel creation and fd passing;
  * ATTACH/DETACH coordination;
  * status and diagnostics.
* **Worker:** exactly one cs7 instance and its transport and protocol state. This boundary is unchanged from Phase
  1/2 (`aworker.c` `aworker_run()`, `cs7_collect()` [V]).
* **Same-binary guarantee:** workers are `bsc-aworker` re-executing its own image, opened at startup, via
  `fexecve()` (today's `aworker_pool_set_exec()` / `child_exec_worker()` mechanism [V]). The standalone executable
  uses its own worker-role argv contract. **The legacy worker argv contract (`--aworker-fd N --aworker-index N
  --aworker-config FILE`) stays unchanged** (N3, APPROVED); P1–P20's fake worker depends on it.
* **Worker-role arguments (decision B4):** `--aworker-fd` and `--aworker-snapshot-fd` are kept, not renamed. They are
  internal (not operator options) and strictly validated. **As implemented** (S2-2, osmo-bsc legacy mode, inline
  form): `--aworker-fd N --aworker-index I --aworker-snapshot-fd M`; M must be above 2, differ from N, and cannot be
  combined with `--aworker-config`; in this mode I must be plain digits 0–15. Every violation exits 2. osmo-bsc lists
  the option in its existing "not for manual use" help section; `bsc-aworker` hides its worker-role options from its
  public help. The legacy form's lenient `--aworker-index` parsing is unchanged.
* **As implemented in S2-3 (`src/a-worker/aworker_main.c`, `bsc-aworker`):**
  * **Supervisor role** (default): `bsc-aworker [-c FILE]` (default `osmo-bsc.cfg`; logging options `-d`, `-s`, `-T`,
    `-e` as in osmo-bsc). It reads the file once with `aworker_pool_supervisor_read_config()`: an `a-worker-pool` with
    at least one worker, every worker inline (D-S1), no `on-worker-failure shutdown` (D-S8), and it parses only its
    view (log targets and `a-worker-pool`; no cs7 instance, nothing of the BSC). A refused configuration exits 1.
    Then it runs the pool exactly as osmo-bsc's legacy mode does -- the same `libaworker` code: snapshots, workers
    re-executing its own image from `/proc/self/exe` (also after the file on disk was replaced), restart policy,
    restart limit, FAILED, startup timeout. SIGTERM or SIGINT: GOODBYE to every worker, collect them, exit 0. If the
    Supervisor is SIGKILLed, its workers end with it (PR_SET_PDEATHSIG). It does not daemonize and uses no systemd
    API.
  * **Worker role** (internal, hidden from `--help`): `--aworker-fd N --aworker-index I --aworker-snapshot-fd M`,
    validated as above, plus `--aworker-incarnation K` (its start number, passed by the Supervisor only; digits, 1 or
    more; B5). `--aworker-config` and `-c` are refused in this role (exit 2). It runs `aworker_run()` in snapshot mode.
  * **Linkage:** only `libaworker` and the Osmocom libraries; no BSC symbol is defined, and every undefined symbol
    resolves in libosmocore, libosmovty, libosmo-sigtran, talloc or libc.
  * **Configuration compatibility:** besides libosmovty's logging commands it registers osmo-bsc's deprecated log
    subsystems (`cc`, `mgcp`, `nat`) and accepts osmo-bsc's `logging filter imsi IMSI` without effect (there are no
    subscribers in the Supervisor or its workers), so that every `log` section osmo-bsc accepts parses here too.
* **Legacy in-process mode** (osmo-bsc as the parent) remains available and is the **default** until S7 passes
  (APPROVED DIRECTION).

## 4. Configuration architecture

### 4.1 Source constraints [V]

1. **The BSC must never parse a worker's `asp`/`as`/`listen` node.** Parsing one creates the ASP and opens its SCTP
   association at once. `include/osmocom/bsc/aworker_cfg_scan.h` documents the paths
   (`ss7_vty_node_asp_go_parent()`, `[no] shutdown` → `osmo_ss7_asp_restart()`), and libosmo-sigtran 2.3.0 cannot
   suppress it.
2. **cs7 commands that create transport** (`libosmo-sigtran` `install_lib_element(L_CS7_NODE, …)`):
   * `asp …` (two forms) and `no asp`;
   * `as …` and `no as`;
   * `listen …` (`ss7_xua_srv_vty.c` `cs7_xua`) and `no listen`.

   All other cs7-node commands are inert configuration: `point-code`, `point-code format`, `point-code delimiter`,
   `secondary-pc`, `network-indicator`, `sccp-address`, timers, `xua` timers,
   `sccp max-optional-data`, `opc-dpc-shift`, `sls-shift`, `permit-dynamic-rkm-allocation`, `description`.
   **Correction (S2-1 follow-up):** `route-table` and `update route` are installed only by `osmo_ss7_vty_init_sg()`
   (`ss7_vty.c`); osmo-bsc and its workers use `osmo_ss7_vty_init_asp()`, so `route-table` is no osmo-bsc command at
   all and is refused ("There is no such command") in any configuration, before any socket is opened. It needs no
   view filtering.
3. **Pool mode never auto-creates an ASP in the BSC.** `osmo_bsc_sigtran.c` `osmo_bsc_sigtran_init()` skips
   `osmo_sccp_simple_client_on_ss7_id()` when the pool is enabled. The existing post-parse check
   `aworker_pool_check_main_cs7()` refuses a main-process cs7 instance with an AS.
4. **libosmovty file parsing** (`command.c` `config_from_file()`, `cmd_make_strvec2()`):
   * one command per line, read with `fgets()` into `VTY_BUFSIZ` = 512;
   * no continuation lines and no quoting: tokens are split on whitespace, so `!` in mid-line is a token, not a
     comment;
   * a comment is a line whose first non-blank character is `!` or `#`;
   * blank lines are skipped;
   * indentation uses spaces/tabs and is compared by exact prefix (`indent_cmp()`);
   * **deeper indentation is legal only directly after a command that entered a child node** (otherwise
     `CMD_ERR_INVALID_INDENT`).

   Indentation therefore mirrors node nesting exactly in every file the parser accepts. `cmd_make_strvec2()` is
   exported (`include/osmocom/vty/command.h`).
5. **Parsing from a stream.** `vty_read_config_filep(FILE*, priv)` parses a stream and does **not** set the config
   path; `vty_read_config_file()` does (`host_config_set()`). On error the parser prints the error kind and the
   offending line **text**, not a line number (`vty.c`). **As implemented:** osmo-bsc parses a pool configuration
   from its single-read buffer and then calls `host_config_set()` itself — for the file form always, for the inline
   form after the BSC view parsed (decision D-8; without it `show startup-config` called `fopen(NULL)`).
6. **A prescan already exists.** `osmo_bsc_main.c` runs `aworker_pool_prescan_config()` (a text pass) **before**
   `vty_read_config_file()`. A text pre-pass is therefore already the established structure.

### 4.2 Design: one file, one pre-pass, standard parser per process [A] (PROPOSED)

Text filtering is kept, but made exact and verifiable. The VTY parser stays the only interpreter of values.

* **Pre-pass (the "view builder").** It extends the existing prescan and lives in `libaworker.la`.
  * It reads `osmo-bsc.cfg` with **the parser's own rules**: `fgets()` with `VTY_BUFSIZ`, `cmd_make_strvec2()` for
    tokens and indentation, and the same exact-prefix comparison.
  * It refuses (as a global configuration error) any file the parser would refuse on indentation grounds, and any
    line of 511 characters or more inside an `a-worker-pool` or worker-owned cs7 section.
  * It first collects the `a-worker-pool` section, whose position in the file does not matter. That determines the
    worker-owned cs7 instances (`worker I cs7-instance X`, §4.5).
* **Subtree rule.** A subtree is a header line plus every following line whose indentation has the header's
  indentation as a proper prefix, as defined by `indent_cmp()`. Because deeper indentation is only legal after a
  node-entering command [V 4.1.4], this is exactly the set of commands executed inside that node. Removing a subtree
  never changes the parent context of a later line: a later line at the header's level or above returns to the same
  ancestor as in the unfiltered file.
* **Line numbers preserved.** Removed lines are replaced by **empty lines**, which the parser skips [V], so every
  view has the same line count as the file.
  * Diagnostics name the process role, the view and the section.
  * The parser prints the offending text; locating its line number would need re-scanning the file for that text.
    Whether to add this is [T] (it is optional diagnostics).
* **Views** (each parsed with `vty_read_config_filep()` on an in-memory stream, `fmemopen()`):

| View | Content |
|---|---|
| BSC | whole file; inside each **worker-owned** cs7 instance the subtrees of `asp`, `as`, `listen` and their `no` forms are removed. Anywhere else these commands are a global error in pool modes that use the inline form. |
| Supervisor | `log …` sections and the `a-worker-pool` section only |
| Worker *i* | `log …` sections, the `a-worker-pool` section, and only the subtree `cs7 instance X` of its worker |

* **Defence in depth against SCTP in the wrong process:**
  1. the view builder removes the transport commands;
  2. the existing prescan rule is re-run **on the BSC view**: no `asp`/`as`/`listen` may remain in any cs7 section;
  3. after parsing, `aworker_pool_check_main_cs7()` [V] refuses any AS in the BSC;
  4. pool mode never calls `osmo_sccp_simple_client_on_ss7_id()` [V].
* **Explicit refusals** (global errors, exit 1 as today's `check_aworker_ownership()` failure in `osmo_bsc_main.c`
  [V]):
  * `exit` or `end` lines inside an `a-worker-pool` or worker-owned cs7 section (they would make node structure
    depend on more than indentation);
  * a worker-owned cs7 instance defined twice;
  * a worker-owned instance without SIGTRAN nodes, or with conflicting protocols (the scanner's existing checks
    [V `aworker_cfg_scan.c`]).
* **Same effective worker configuration everywhere.** The Supervisor builds each worker view **once**, at its
  start. Workers parse exactly that snapshot (§4.4). The BSC's file is checked against the Supervisor's at
  SESSION_OPEN (§4.6).

**Why not alternatives** (considered; VERIFIED infeasible or worse):
* **Suppressing ASP creation inside the parser:** not possible in libosmo-sigtran 2.3.0 [V 4.1.1].
* **The BSC installing dummy commands for `asp`/`as`/`listen`:** these commands are installed by
  `osmo_ss7_vty_init_asp()`, which the BSC needs for `point-code` and `sccp-address` on the same node. Installing a
  second command with the same syntax is not supported.
* **Separate files per process:** excluded by the single-source requirement.

### 4.3 Robustness tests the view builder must pass (S2 gate)

Golden-file tests compare the views and the parse result of each view with expectations, including:
* nested nodes (`asp` children, `sccp-address` children, `as` children);
* comments at every depth, including `!` inside a token;
* blank lines;
* tabs, spaces and mixed-but-consistent indentation;
* inconsistent indentation (must be refused exactly like the parser refuses it);
* a cs7 section at the end of the file;
* an `a-worker-pool` section before and after the cs7 sections;
* a worker-owned instance containing only inert commands;
* a non-worker cs7 instance with `asp` (refused);
* overlong lines;
* `exit`/`end` lines;
* duplicate instance sections.

Equivalence test: for a legacy two-file configuration and its inline equivalent, the BSC's parsed objects and each
worker's parsed objects are identical. Comparison is by `show running-config`-style output of the relevant nodes [T].

Per the S2-0 review (clarification B), comparing views with their own expected text is not enough. Every generated
view is also parsed by the **real** libosmovty / libosmo-sigtran parsers, and the resulting configuration objects are
checked. In particular, no ASP or AS exists after parsing the BSC view. See plan §14.3 S2-1.

### 4.4 Supervisor snapshot (D-S2) [A]

* The Supervisor reads `osmo-bsc.cfg` **once**, at startup, and keeps the views as immutable in-memory text for its
  lifetime.
* For each worker it creates a **sealed anonymous file** holding that worker's view (`memfd_create()` with
  `F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL`; if no sealed memfd can be made, an unlinked file in a
  private directory). The descriptor is inherited by every spawn; the worker reads it completely, closes it and
  parses the buffer (details as implemented in S2-2 below).
* **Workers never open `osmo-bsc.cfg`.** Restarts reuse the snapshot.
* **Runtime ownership validation stays** (2a-3b): the Supervisor still checks every CS7_INFO against the ownership
  map (`ownership_claim_ok()` [V]). Its purpose in snapshot mode is to catch divergence between the view builder's
  ownership map and what the libosmo-sigtran parser actually created from the view, for example a builder or parser
  disagreement or a parse that silently yields another instance. It remains worker-local and leads to
  FAILED(ownership).
* **Legacy file form** (`worker I config-file FILE`, legacy mode only): unchanged. Workers re-read their files at
  restart, and 2a-3b keeps its original scenario (file edited at runtime → FAILED(ownership)).
* **As implemented in S2-2 (legacy mode, inline form; `aworker_snapshot.c`, `aworker_pool.c`, `aworker.c`):**
  * osmo-bsc reads `osmo-bsc.cfg` once (`aworker_cfg_read_stable()`: one open, metadata compared before and after,
    a second byte-compared pass; at most 16 MiB). The model, the BSC's parse, the BSC view and every snapshot come
    from that buffer. This detects changes during the read on a best-effort basis; it is **not** an atomic
    filesystem snapshot.
  * Before anything is forked, the running pool configuration must declare exactly the workers of the model read at
    startup (count and cs7 instance per worker), and must come from such a read at all; otherwise the pool does not
    start. Each worker view is then stored once, before the first fork, in a memfd named `bsc-aworker-<i>` created with `MFD_CLOEXEC|MFD_ALLOW_SEALING|MFD_NOEXEC_SEAL`
    (retried without `MFD_NOEXEC_SEAL` on `EINVAL` only) and sealed with `F_SEAL_SHRINK|GROW|WRITE|SEAL`; all four
    seals must be present or the memfd is not used. Size 1 octet … 1 MiB. Verified here: kernel 7.0.0-43,
    `vm.memfd_noexec` 0, `MFD_NOEXEC_SEAL` accepted, seals 0x2f (exec seal included).
  * **Fallback** only if no sealed memfd can be made: an unlinked, mode 0400 file on a read-only descriptor, made
    through a private 0700 directory `bsc-aworker-snap.<random>` in the first usable of `$RUNTIME_DIRECTORY` (first
    entry, absolute only), `/dev/shm`, `/tmp` (never `$TMPDIR`). A candidate is usable if owned by the effective
    user (any mode) or root-owned and sticky; anything else (symlink, not a directory, other owner) is refused, and
    with no usable candidate there is no snapshot and the pool does not start. **It is weaker than a sealed memfd:**
    another process of the same user can reopen it through `/proc/<pid>/fd`, make it writable and change it, or
    read it; root can do anything. Its use is logged as a WARNING saying so. Leftover private directories are
    reported, never removed.
  * Before every start (first, restart, reset) the snapshot is re-checked against the copy in memory; a changed
    snapshot makes the worker FAILED (config) instead of starting it.
  * The child keeps only stdio, its IPC socket, its own snapshot and the close-on-exec executable descriptor, and
    closes everything else, including the other workers' snapshots. After exec a worker holds stdio, its IPC socket
    and its snapshot; it reads the snapshot completely with `pread()` from offset 0, closes it, checks the view as
    text (D-13: its own declaration, exactly its own cs7 instance with transport, no `log file`, no `exit`/`end`,
    indentation and size rules), and only then parses the buffer (`vty_read_config_filep()` on `fmemopen()`, not
    `fdopen()`). A snapshot refused by the read or by the D-13 check makes the worker exit 66 (`AWORKER_EXIT_CONFIG`)
    before any parse, so before an ASP or socket exists (verified with strace on the real binary, R-7). After the
    parse, the cs7 instances with transport must be exactly the declared one, and the CS7_INFO ownership check stays;
    both refuse after the parse, i.e. after the parser has created the ASP.
  * Worker views omit `log file` sections (D-2): workers log to the stderr they inherit; they never open the BSC's
    log file.
  * A reset after FAILED reuses the snapshot made at startup and logs that a changed file takes effect only after
    restarting osmo-bsc (D-7). Verified with the real binary: after `osmo-bsc.cfg` was moved away, replaced or
    deleted, restarted workers reported their original configuration and no process opened the file (R-6).
* **Activating a new configuration:**

| Change | What is needed | Transport |
|---|---|---|
| BSC-only (network, BTS, msc, BSC timers, logging of the BSC) | restart `osmo-bsc` | preserved |
| `a-worker-pool` pool-relevant keys or any worker-owned cs7 subtree | stop the BSC, restart the Supervisor (new pool), start the BSC; or restart the Supervisor first: the running BSC loses its session, reconnects and is rejected on mismatch (§4.6) | interrupted (intended) |
| editing the file without restarting anything | no effect on the running pool; a BSC restarted later is rejected if the pool-relevant parts differ | — |

### 4.5 Inline form and legacy compatibility (D-S1) [A]

* **Syntax: `worker <0-15> cs7-instance <0-15>`** in `a-worker-pool` (implemented in S2-1). It differs from the existing
  `worker <0-15> config-file FILE` in its second keyword, so the VTY grammar is unambiguous [V: distinct literal
  tokens].
* **Conflict rules** (checked after parsing, because VTY command order is free):
  * A worker has exactly one form. `config-file` and `cs7-instance` on the same worker is a global error.
  * A pool uses **one** form for all its workers. Mixed forms are a global error. This keeps one source of truth
    for ownership and one rule set for where ASP nodes may appear.
  * **Inline pools:** `asp`/`as`/`listen` are allowed only inside worker-owned instances (filtered for the BSC) and
    refused in any other cs7 instance.
  * **File-form pools:** `asp`/`as`/`listen` anywhere in `osmo-bsc.cfg` are refused, as today
    (`aworker_pool_prescan_config()` [V]).

  Either way an ASP is defined in exactly one place and parsed by exactly one process, so no duplicate SCTP
  association is possible.
* **Supervisor mode accepts only the inline form.** The file form is a global error there (single source).
* **Legacy mode** accepts both forms. The file form is **deprecated**: it is to get a deprecation notice at startup
  and in the VTY help. **Not implemented yet:** at `b4d890241` neither the startup log nor the VTY help says so. In
  legacy mode with the inline form, the osmo-bsc process builds the worker snapshots itself, using the same
  view-builder code (implemented in S2-2, §4.4).
* **Configuration writes in inline mode (S2-1 Option C, implemented):** osmo-bsc holds only the BSC view, so every
  write form is refused with the reason: VTY `write file [PATH]`, `write`, `write memory`, `copy running-config
  startup-config` (through libosmovty's `config_is_consistent` hook, before PATH is recorded) and CTRL
  `write-config-file` (`overwrite` or PATH). `write terminal` / `show running-config` still show the BSC view.
  Verified with the real binary in normal and ASan builds (S2-2 R-2), also with the config path recorded (D-8).
* There is no implicit precedence anywhere: every ambiguity is an error.

### 4.6 Canonical effective configuration and comparison (D-S3, D-S4, D-S5) [A]

**What is compared (D-S3).** A canonical document with three parts, built by shared code in both processes:

| Part | Content | How the BSC obtains it |
|---|---|---|
| H | header line `aworker-canonical-config 1` (representation version) | constant |
| P | **effective pool definition.** Every Supervisor- and worker-relevant `a-worker-pool` value **after parsing**, with defaults **explicit**, in a fixed key order. Covered: mode, supervisor-socket, workers, per worker (ascending index) its cs7-instance, startup-timeout, restart-delay, restart-delay-max, stable-time, max-restarts, window, on-worker-failure, worker echo-interval and echo-misses, session echo parameters, protocol-timeout, detach-release-timeout. Integers in decimal, enumerations by their canonical VTY keyword. **Excluded:** BSC-only keys (`drain-timeout`, and future admission keys), so BSC-only tuning never forces a pool restart. | parses `a-worker-pool` (shared `aworker_vty.c`) as it does today |
| C | **worker-owned cs7 subtrees**, in ascending worker index. Each command is one line: depth as a decimal integer, then the tokens from `cmd_make_strvec2()` joined by single spaces. Comments, blank lines and indentation characters are dropped; command order is preserved, because VTY order is significant. Includes `asp`/`as`/`listen` and every other command of the subtree. | builds it from text with the view builder; it does **not** parse the asp nodes |

* **Limitation of C (VERIFIED constraint).** C is *syntactic*, not fully effective. Defaults omitted in the file
  stay implicit, and equivalent spellings (for example a point code written in another format) count as different.
  Effective normalisation of cs7 values would need a process that executes those commands, and executing `asp`
  opens SCTP (§4.1.1). Only the worker can do that. The consequences:
  * a difference in spelling causes a **false mismatch**, which is safe (refusal), never a false match;
  * effective cs7 facts are cross-checked at runtime. CS7_INFO carries the primary point code, protocol and AS
    count; the Supervisor checks ownership, and the BSC checks the point code against its msc address
    (`osmo_bsc_sigtran.c` `a_worker_available()` [V]);
  * PROPOSED addition: the worker reports a digest of its instance's effective configuration, as written back by
    libosmo-sigtran's own config writer, in STATE. The Supervisor compares it across restarts of that worker
    (diagnostics) [T: writing one node's running configuration to a buffer must be shown feasible with libosmovty
    1.14.2].
* **Status after the S2-0 review:** D-S5 is APPROVED as a direction (deterministic canonical-document comparison,
  not hash-only), **subject to defining the document's exact contents (CD-1, open, must be resolved before S2-5).**
  CD-1 must specify: explicit values; effective defaults; normalized ordering and representation; per-worker cs7 and
  transport configuration; pool and Supervisor settings; and how semantically identical configurations with
  different formatting or spelling are handled. **Raw configuration files are not compared.** Note the tension
  recorded for CD-1: part P below is effective, but part C is syntactic (normalized tokens, not raw text) because
  only the worker process can execute the cs7 transport commands (§4.1.1). CD-1 must either accept that, with its
  false-mismatch consequence, or define a different effective representation.
* **Comparison method (D-S5): exact comparison of the whole canonical document, no hash.**
  * In SESSION_OPEN the BSC passes its canonical document as a sealed memfd via SCM_RIGHTS. The same mechanism is
    needed anyway (§8), and the document can exceed the 4096-octet IPC frame (`AWORKER_IPC_MAX_FRAME` [V]).
  * The Supervisor compares it byte for byte with its own start-time document.
  * On mismatch, SESSION_REJECT(config-mismatch) names the first differing part, worker and line, which a hash cannot
    do.
  * Exact comparison has no collisions and needs no cryptographic dependency.

  Alternatives if a compact identifier is wanted later (for logs or status):

  | Option | Facts | Assessment |
  |---|---|---|
  | libosmocore/libosmogsm SHA-256 | libosmogsm compiles an internal SHA-256 (`src/gsm/kdf/sha256*.c`) but exports only `osmo_kdf_*` [V `nm -D`]; libosmocore exposes no hash API | not usable without a libosmocore change (pinned) |
  | Nettle `sha256_*` | `nettle` is installed in this build environment (`pkg-config --list-all`) [V]; osmo-bsc's `configure.ac` has no crypto dependency today [V] | best option *if* a hash is required: established, small, LGPL; adds a build dependency (DevOps/packaging impact) |
  | Bundled SHA-256 source | duplicates code that libosmogsm already carries privately | not recommended |
  | GnuTLS | libosmocore here is built without it (`ldd`) [V]; heavier | not recommended |

* **Mismatch handling (D-S3, APPROVED DIRECTION; detail PROPOSED).** A mismatch is a global session error. The
  Supervisor rejects. The BSC treats it like any other global configuration error: it logs both parts that differ
  and exits with code 1, as for today's failed ownership check (`osmo_bsc_main.c` `check_aworker_ownership()` →
  `exit(1)` [V]). The same applies at startup and on reconnect after a Supervisor restart.
* **Protocol vs configuration compatibility** are separate:
  * protocol: IPC major/minor version (§8.2), `SESSION_REJECT(version)`;
  * configuration: canonical document, `SESSION_REJECT(config-mismatch)`.

  The representation version in H changes when the canonical format changes. A different H is a configuration
  mismatch with its own message.
* **Legacy forms** are never compared. Supervisor mode accepts only the inline form, and legacy mode has no session.

**As implemented (S2-5; CD-1 decided 2026-11-18 as proposed above; `aworker_canon.h`):** H `aworker-canonical-config
1`; P lines `P <key> <value>` for workers, each `worker I cs7-instance X`, `supervisor-socket` (or `none`),
startup-timeout, restart-delay, restart-delay-max, stable-time, `max-restarts N window S`, on-worker-failure,
session-echo-interval and session-echo-misses -- the keys that exist; keys proposed above but not implemented yet
(mode, worker ECHO, protocol-timeout, detach-release-timeout) join with a new representation version; C lines
`C <worker> <depth> <tokens>` from `cmd_make_strvec2()`. Built from what each process parsed
(`aworker_pool_canon_document()`); both readers -- the Supervisor's and osmo-bsc's inline read -- give the same document
(tested). SESSION_OPEN passes it as a sealed memfd; the Supervisor compares exactly and names the first differing line
and part.

### 4.7 Configuration ownership table

| Category | Authoritative source | Parsed by | Consumers | Shared / per worker | Consistency check |
|---|---|---|---|---|---|
| BSC network, BTS, MSC, BSC timers, `line vty`, `ctrl`, BSC-only pool keys (`drain-timeout`) | `osmo-bsc.cfg` | BSC | BSC | BSC only | none needed (not part of the pool) |
| Inert cs7 values of worker-owned instances (`point-code`, formats, `sccp-address` book) | `osmo-bsc.cfg` `cs7 instance X` | BSC (BSC view) and worker *i* (worker view) | BSC (msc/local addresses), worker (SS7 instance) | one definition per instance | canonical part C; runtime CS7_INFO point-code check [V] |
| Transport cs7 commands (`asp`, `as`, `listen`, …) of worker-owned instances | `osmo-bsc.cfg` | worker *i* only | worker | per worker | part C; view-builder and prescan checks (§4.2) |
| Pool membership `worker I cs7-instance X` | `osmo-bsc.cfg` `a-worker-pool` | BSC, Supervisor, workers | Supervisor (spawn, map), BSC (msc → instance check) | shared | part P; global checks in both processes |
| Mode, `supervisor-socket` | same | BSC, Supervisor | both | shared | part P |
| Lifecycle (`startup-timeout`, `restart-delay`, `restart-delay-max`, `stable-time`, `max-restarts … window …`, `on-worker-failure`) | same | Supervisor (BSC accepts the syntax) | Supervisor | shared values, per-worker state | part P |
| Worker ECHO, protocol timeout | same | Supervisor | Supervisor | shared | part P |
| Session ECHO | same | Supervisor, BSC | both | shared | part P |
| `detach-release-timeout` | same | worker | worker | shared | part P |
| Logging (`log …`) | `osmo-bsc.cfg` | every process (its view) | each process | shared definitions, per-process targets (§7) | not compared |

Runtime state is never persisted: FAILED, drained, sessions, incarnations, counters.

### 4.8 Proposed `osmo-bsc.cfg` example

Comments must be on their own lines in Osmocom configuration files [V 4.1.4]. Every command directly below a
`! [proposed]` line is proposed syntax; all other commands are existing syntax, verified in `aworker_vty.c`
(a-worker-pool) and in libosmo-sigtran / libosmocore (cs7, log, line vty).

```
log stderr
 logging filter all 1
 logging print category 1
 logging print thread-id 1
 logging level set-all notice
!
line vty
 bind 127.0.0.1
!
! Owned by worker 0. Its asp is parsed by worker 0 only; the BSC parses point-code and sccp-address.
cs7 instance 0
 point-code 0.23.3
 sccp-address msc0
  routing-indicator PC
  point-code 0.23.1
 asp asp-a0 2905 0 m3ua
  remote-ip 10.0.0.10
  local-ip 10.0.0.1
  role asp
  transport-role client
  no shutdown
!
! Owned by worker 1: a second A link with its own BSC point code.
cs7 instance 1
 point-code 0.23.4
 sccp-address msc0b
  routing-indicator PC
  point-code 0.23.1
 asp asp-a1 2905 0 m3ua
  remote-ip 10.0.0.10
  local-ip 10.0.0.1
  role asp
  transport-role client
  no shutdown
!
a-worker-pool
! [proposed] absent: legacy in-process mode
 mode supervisor
! [proposed]
 supervisor-socket /run/osmocom/bsc-aworker.sock
 workers 2
! inline form (implemented in S2-1); replaces 'worker <0-15> config-file FILE' (deprecated, legacy mode only)
 worker 0 cs7-instance 0
 worker 1 cs7-instance 1
 startup-timeout 60
 restart-delay 5
 restart-delay-max 60
 stable-time 60
 max-restarts 5 window 300
 on-worker-failure restart
! [proposed] 2a-6 worker ECHO, approved defaults
 echo-interval 2
 echo-misses 3
! [proposed] session ECHO; values are placeholders to be tuned (D-S12), not defaults
 session-echo-interval 2
 session-echo-misses 3
! [proposed] 2b readiness, approved default
 protocol-timeout 60
! [proposed] 2d, approved default (BSC-only key)
 drain-timeout 300
! [proposed] no default yet: decided from the S5 protocol tests (D-S9); without it the pool refuses to start
 detach-release-timeout-ms <to be determined>
!
network
 ! unchanged
msc 0
 msc-addr msc0
 ! unchanged; several A links per MSC need the approved 2c lookup change (D2)
```

Invocation (development and testing):
* `bsc-aworker -c /etc/osmocom/osmo-bsc.cfg` (Supervisor);
* `osmo-bsc -c /etc/osmocom/osmo-bsc.cfg` (BSC).

## 5. Operational interfaces (D-S6, D-S7)

### 5.1 Findings [V]

* `vty_init()` → `cmd_init(1)` (`vty.c`) installs `configure terminal`, `copy running-config startup-config` and
  `shutdown` on the enable node.
* `install_basic_node_commands()` (`command.c`) installs `write terminal`, `write file [FILENAME]`, `write memory`,
  `write` and `show running-config` on every node.
* `struct vty_app_info` has no read-only or "no config" option.
* `write file FILENAME` sets the config path from its argument (`config_write_file`). A plain `write` refuses only
  while `host.config` is NULL. (osmo-bsc's inline mode does not rely on that: it refuses every write through the
  `config_is_consistent` hook, §4.5.)

So a read-only libosmovty VTY is **not possible** in 1.14.2. There is no mechanism to disable or safely override
configuration writing; overriding by installing a second command with the same syntax is not supported.

### 5.2 Decision (D-S6, APPROVED): no runtime management VTY server

Two different things must not be confused:

* **Registering VTY commands for configuration parsing.** The Supervisor (and the worker role) calls `vty_init()` and
  installs the node and command definitions it needs (`a-worker-pool`, logging, and in the worker role the cs7
  nodes), and parses its view with `vty_read_config_filep()`. This is **required** and stays. `vty_read_config_filep()`
  parses through an internal `VTY_FILE` vty and does not need a telnet server [V `vty.c`].
* **Starting a runtime VTY server.** The Supervisor **never** calls `telnet_init_default()` / `telnet_init_dynif()`
  (**as implemented** in S2-3: the real binary's process test finds no network socket at all in the Supervisor).
  There is no interactive VTY, so `configure terminal` and `write file FILENAME`, which libosmovty installs
  unconditionally (§5.1), cannot be reached. The partial-overwrite hazard does not arise. Verified in S2-3 by a test
  that the Supervisor opens no listening TCP socket.

Operational access (runtime state and actions only, never configuration):

* **Through the BSC** while attached: `show a-worker-pool` and `a-worker-pool reset worker N` on the BSC VTY,
  forwarded over the session (S2-7).
* **Operator control channel** on the Supervisor's Unix socket, with STATUS, RESET_WORKER, FENCE_SESSION and
  POOL_SHUTDOWN, and a CLI client mode of the executable (`bsc-aworker --control PATH …`, syntax PROPOSED). It is
  implemented in **S2-5**, together with the socket security policy **N5 (§9.4)**. **As implemented:** OP_REQUEST /
  OP_RESULT on the session socket (`supervisor-socket`); `bsc-aworker --control PATH status | reset-worker N |
  fence-session | pool-shutdown`, exit 0 / 1 (refused) / 2 (usage, no connection) / 3 (this client refused). No
  operation changes the configuration. Without `supervisor-socket`, signals only, as before.
* **Diagnostics:** logging (§7).
* A libosmoctrl CTRL interface may be added later; it needs a TCP port (§5.3).

### 5.3 Port (D-S7)

`include/osmocom/vty/ports.h` (libosmocore 1.14.2) assigns no port for an A-worker supervisor [V]. Osmocom ports are
assigned centrally. Because the MVP has no Supervisor VTY or CTRL, **no port is needed**. If a TCP interface is added
later, the port becomes a deployment and registry decision. No port is reserved or invented here.

## 6. `on-worker-failure` (D-S8) (APPROVED DIRECTION; VERIFIED feasible)

* **Legacy mode:** unchanged. `shutdown` ends osmo-bsc on any worker failure (`worker_apply_policy()` SHUTDOWN
  branch [V]).
* **Supervisor mode:** `on-worker-failure shutdown` is a **global configuration error**, refused at startup by both
  the Supervisor and the BSC, which both parse `a-worker-pool`. A worker failure affects only that worker (restart
  policy, FAILED). Only `restart` is valid.

## 7. Logging (D-S10) (APPROVED with limitations, decision B2 of 2026-11-17)

**As implemented in S2-3:** the Supervisor keeps the `log file` targets of its view; its workers hold no descriptor of
such a file and log to the stderr they inherit (tested with the real binary). Identity (B5), in `bsc-aworker` only:
the Supervisor's pool lines begin `aworker-supervisor: `, a worker's `aworker <idx> inc <n>: `; messages and the IPC
HELLO name `bsc-aworker`. osmo-bsc's output is unchanged. The durable shared-file regression test is
`tests/aworker_logfile` (4 writers, 2,500 lines each of 10–4,000 octets, default and `blocking-io`; it fails if the
writers did not really interleave); `wq` stays untested.

**Decision B2 (authoritative; it replaces the proposal below where they differ):**
* Workers follow D-2: their views contain no `log file` target, they open no log file of their own, and they log to
  stderr, inherited from their parent (the Supervisor; in today's legacy mode osmo-bsc). **Implemented and verified
  in S2-2** for legacy mode (worker view rule, D-13 refusal, real-binary test R-4: a worker has no descriptor of the
  BSC's log file, its lines are on stderr and not in the file).
* The Supervisor keeps its configured logging targets, file targets included. The BSC keeps its logging unchanged.
* Supervisor and BSC may append to the same configured file, within the tested limits below.
* Identity prefixes (`aworker-supervisor`, `aworker <idx> inc <n>`) for the new executable only (decision B5); the
  legacy osmo-bsc log output and categories stay unchanged.

**Evidence and limits (the concurrent shared-file experiment, accepted as evidence):** 4 processes, each 20,000 lines
of 10 to 4,000 octets, through libosmocore 1.14.2's file target into one file, 3 runs per mode, on ext4, kernel
7.0.0-43: 80,000 lines per run, 0 malformed, every line present once, in both the default (`nonblocking-io`) and
`blocking-io` modes. **`wq` mode was not tested: no atomicity or correctness is claimed for it.** Other filesystems,
kernels, line lengths and process counts are untested. A durable regression test is still required (S2-3); the
experiment itself is not part of the test suite.

Findings [V]:
* osmo-bsc's category table is `static const struct log_info_cat osmo_bsc_categories[]` in `osmo_bsc_main.c`, and
  the log configuration lives in `osmo-bsc.cfg` `log …` sections.
* `logging print thread-id 1` prints the thread id on every line (`logging.c`); for these single-threaded processes
  that is the PID.
* `log file FILENAME [(nonblocking-io|blocking-io|wq)]` (`logging_vty.c` `cfg_log_file`). **Corrected:** both the
  default `nonblocking-io` and `blocking-io` use a stdio stream opened with `"a"` (`O_APPEND`), written with
  `fputs()` + `fflush()` per message; they differ only in `O_NONBLOCK`. Only `wq` writes through osmo_io
  (`logging_file.c` `log_target_file_switch_to_stream()` / `_switch_to_wqueue()`).
* SIGHUP reopens log targets per process (`osmo_init_ignore_signals()` installs the handler).
* Today a worker inherits the BSC's stdout/stderr and logging command-line options (`aworker_pool.c` `is_log_opt()`
  [V]).

Proposal (as reviewed before B2; superseded where B2 differs):

* **One definition, per-process targets.**
  * The `log …` sections of `osmo-bsc.cfg` are part of every view. Each process (BSC, Supervisor, worker) creates
    its **own** targets from them. The BSC never configures logging for the Supervisor or workers.
  * The category table moves unchanged into shared code (S1), so `logging level <category> …` lines parse
    identically everywhere. The BSC-specific filter function stays in osmo-bsc.
* **Workers log independently of attachment.** They configure their targets from their snapshot at startup, so
  they log before and between BSC sessions.
* **Identity in every line.**
  * The code prefixes Supervisor messages with `aworker-supervisor` and worker messages with
    `aworker <idx> inc <incarnation>`; today it is `aworker %u` [V `LOGAW`].
  * The PID comes from `logging print thread-id 1` (recommended in the operator documentation, shown in §4.8).
* **Destinations:**
  * stderr of the Supervisor and workers is the Supervisor's stderr, never the BSC's, so it survives BSC restarts.
  * syslog and GSMTAP targets are multi-process by nature.
  * A **shared `log file`** is written by several processes. See the B2 evidence above: whole lines in the default
    and `blocking-io` modes under the tested conditions; `wq` untested. (The earlier statement here that
    `blocking-io` splits lines was not borne out by the test and is withdrawn.)
  * If separate files per role are wanted, that would need per-role keys, i.e. duplicated settings. It is **not**
    proposed for the MVP.
* **After a BSC restart:** Supervisor and worker logs continue unchanged. The new session_id appears in the
  Supervisor's attach messages, so log lines can be correlated across sessions.

## 8. IPC and fd passing [A]

### 8.1 Channels

| Channel | Ends | Lifetime | Messages |
|---|---|---|---|
| Control | Supervisor ↔ worker (socketpair at spawn, as today) | worker process | HELLO, CS7_INFO, ECHO_REQ/RSP, GOODBYE, ATTACH(session_id, fd), DETACH(session_id), STATE |
| Data | BSC ↔ worker | one BSC session; **fresh socketpair per worker per session** | HELLO (session-bound), USER_BIND/UNBIND, BIND_RESULT, TX_*, SCU_UP, TX_FAILED, ECHO (link level) |
| Session / operator | BSC or operator CLI ↔ Supervisor, `AF_UNIX SEQPACKET` at `supervisor-socket` | session / request | SESSION_OPEN(+fd: canonical document), SESSION_ACCEPT/REJECT/CLOSE, WORKER_UP(idx, incarnation, fd), WORKER_DOWN(idx, incarnation, reason), WORKER_STATE, ECHO, RESET_WORKER, STATUS; operator: STATUS, RESET_WORKER, FENCE_SESSION, POOL_SHUTDOWN |

Each existing message type is assigned to exactly one class. A record of a class not allowed on a channel closes
that channel (fail closed, like `aworker_ipc_dec()`'s role rules [V]).

**As implemented (S2-4):** `aworker_ipc_msg_class()`: LINK (HELLO, ECHO_REQ, ECHO_RSP; carried by every link),
CONTROL (GOODBYE, CS7_INFO, ATTACH, DETACH), DATA (USER_BIND, USER_UNBIND, USER_BIND_RESULT, TX_*, SCU_UP, TX_FAILED),
SESSION (S2-5: SESSION_*, WORKER_*, OP_*; STATE is CONTROL). A link carries LINK plus `cfg->classes`, by default
CONTROL and DATA: osmo-bsc's worker link carries both, as before. **S2-5:** a `bsc-aworker` worker's link to the
Supervisor carries CONTROL only, its data link per session DATA only, and the session socket's links SESSION only. A
received record of another class closes the link (`class`); sending one is refused (-EINVAL).

### 8.2 Identifiers and versions

* **Protocol version:** major/minor in HELLO and SESSION_OPEN. Major must match; minor additions must be optional.
  Replaces `AWORKER_IPC_VERSION 1` exact match [V].
* **Boot nonce:** 64 bit random per Supervisor start.
* **Incarnation:** boot nonce + per-spawn counter.
* **Session ID:** boot nonce + per-session counter, never reused within one boot.

**As implemented (S2-4, protocol 2.0):** the header's version octet is the **major** version (2; exact match). HELLO
appends, after its 1.x fields, `minor u8 | reserved u8 | reserved u16 | boot_nonce u64 | incarnation u32 |
session_id u32`; a HELLO of a higher minor may append more, which is ignored, while trailing octets from the own or a
lower minor are refused. The boot nonce and the per-spawn / per-session counters are separate fields: an incarnation
or session is identified by (boot_nonce, counter). 0 means "not asserted"; a link refuses a peer HELLO only when both
sides assert a field and the values differ (`incompatible`). The pool (osmo-bsc's and `bsc-aworker`'s) asserts a
random boot nonce per pool and each worker start's incarnation; a `bsc-aworker` worker asserts the incarnation its
Supervisor passed (`--aworker-incarnation`), so a mismatch is refused by both ends; osmo-bsc's workers assert nothing.
The session_id field exists; sessions that fill it are S2-5. The SESSION_OPEN version fields are S2-5.

### 8.3 fd passing on the existing link [V findings, A design]

Findings [V]:
* `aworker_ipc_link.c` already uses osmo_io `OSMO_IO_FD_MODE_RECVMSG_SENDMSG`, sends with
  `osmo_iofd_sendmsg_msgb()`, and closes on `MSG_TRUNC`.
* `osmo_iofd_set_cmsg_size()` sets the receive control buffer (`osmo_io.c`). The poll backend passes
  `msg_controllen = cmsg_size` and calls `recvmsg(fd, hdr, msghdr->flags)` with flags 0 (`osmo_io_poll.c`), and
  there is **no API for `MSG_CMSG_CLOEXEC`**.
* `osmo_iofd_sendmsg_msgb()` **copies the control bytes at enqueue time**: `memcpy` of `msg_control` into the queued
  msghdr (`osmo_io.c`). The kernel duplicates the descriptor only when the queued `sendmsg()` actually runs, later.

Design:
* **Sizing.** At most one descriptor per message. Receive buffer `CMSG_SPACE(sizeof(int))`, set only on links that
  may carry descriptors (data-link setup on the BSC, ATTACH on the worker, SESSION_OPEN on the Supervisor).
* **Sender ownership.** The sender keeps its descriptor open **until the send-completion callback**
  (`sendmsg_cb`), and closes it there. It also closes it when the link is freed with the record still queued. Closing
  at enqueue would let the kernel send a closed or reused descriptor number. The sender tracks pending descriptors
  per sequence number.
* **Receiver.** In the receive callback (the `msghdr` is valid only during the callback [V `osmo_io.h`]):
  * Reject `MSG_CTRUNC`: close every received descriptor, then the link.
  * Accept only `SOL_SOCKET`/`SCM_RIGHTS` with exactly the number of descriptors the message type requires (0 or 1).
    Too many, too few, or descriptors on a type that takes none → close all received descriptors, then the link.
  * Immediately set `FD_CLOEXEC` with `fcntl()`, since recvmsg flags cannot be set. This is safe because the
    processes are single-threaded and do not fork between receive and `fcntl()`.
  * **Exactly-once consumption:** the descriptor is handed to exactly one owner (a new link), or closed on every
    failure path.
* **Validation before use.** WORKER_UP / ATTACH carry (session_id, incarnation, idx). A descriptor for an unknown,
  stale or retired session, or for a wrong incarnation, is closed without being used.
  * The data HELLO repeats session_id and incarnation, and both ends check it.
  * The first record must be HELLO with seq 1, and sequence numbers increase by exactly 1 [V link rules].
* **Supervisor side.**
  * For each attach it creates a socketpair and sends one end to the worker (ATTACH) and one to the BSC
    (WORKER_UP). It closes its own copies in the completion callbacks.
  * If either send fails, it closes both copies and sends DETACH to the worker if ATTACH went out. The attach is
    retried for that worker only.
* **Cleanup on EOF or fencing.**
  * When a session closes, workers close their data descriptors (DETACH), and the BSC side closes its copies on
    link EOF.
  * Descriptors still queued on a closed link are closed by their owner as above.
  * Fencing revokes the session first (§9); the killed BSC's descriptors are closed by the kernel.

**As implemented (S2-4; `aworker_ipc_link.c`):**
* ATTACH (main → worker, CONTROL: session_id u32, incarnation u32, both non-zero) takes exactly one descriptor;
  DETACH (session_id) and every other type none (`aworker_ipc_msg_fds()`). Nothing sends them before S2-5.
* Sender: `aworker_ipc_link_send_fd()` takes the descriptor over in every case and keeps it until the record's
  send-completion callback, then closes it; a record that cannot be queued, or that is discarded because the link
  closes or is freed first, has its descriptor closed then. Sending a type with the wrong descriptor count is refused.
* Receiver (`cfg->fd_passing`; control buffer `CMSG_SPACE(sizeof(int))`): every received descriptor is made
  close-on-exec at once; `MSG_CTRUNC`, a control message other than SCM_RIGHTS, a count other than the type's, and
  every other receive error of such a record close all received descriptors, then the link (`rx-fd`). On 64-bit
  Linux that buffer has room for two descriptors through alignment: a second one is caught by the count check, a
  third by `MSG_CTRUNC` -- both tested. The descriptor reaches `rx_cb` in `msg->fd`; `aworker_ipc_link_take_fd()`
  takes it, otherwise the link closes it when `rx_cb` returns.
* Validation against session and incarnation before use, the Supervisor's socketpair per attach and the EOF rules of
  §8.3a are S2-5.

### 8.3a Endpoint ownership and EOF propagation

The exact ownership of every socket endpoint and passed descriptor, and the rule that makes a BSC death visible as EOF
at the worker, are specified in plan §14.3 (S2-5, table "Endpoint ownership"). In short:
* after a descriptor is passed with SCM_RIGHTS, every copy that is not needed is closed: the Supervisor closes its
  copies in the send-completion callback;
* no data endpoint is inherited by any other process (`SOCK_CLOEXEC`; `child_close_fds()` [V]);
* closing the BSC's descriptor yields EOF at the worker only if **no other process holds a duplicate**. This is
  therefore a tested requirement (real process-level test after BSC SIGKILL), not an assumption;
* cleanup of failed, deferred, truncated and partially received transfers is defined in plan §14.3 (S2-4).

### 8.4 Stale-session rejection

It is structural:
* a data link exists for exactly one session;
* the worker closes it on DETACH before it accepts a new ATTACH;
* no record can precede the session-binding HELLO, and none can arrive out of sequence.

No per-record session field is added [A].

## 9. Session state machine, health and fencing (D-S12) [A]

### 9.1 One idempotent session state machine (Supervisor)

```
 NONE ──SESSION_OPEN ok──► ACTIVE ◄──echo ok── UNRESPONSIVE
                             │  ──echo misses──►     │
 SESSION_CLOSE │ EOF │ error ┼───────────────────────┼──────► CLOSING ──all workers DETACHED/gone──► CLOSED
                             │                       │           ▲
           FENCE_SESSION ────┴───────────► FENCING ──┘           │
                                            │ pidfd exit / EOF ──┘
                                            │ exit not confirmed within bound → FENCE_FAILED (stays FENCING;
                                            │ nothing admitted; operator must resolve)
```

* **CLOSING entry action, exactly once:**
  * stop session ECHO;
  * close the session socket;
  * DETACH(session_id) to every worker attached to it;
  * retire the session_id.

  Every later EOF, CLOSE, fence result or ECHO event is ignored in CLOSING/CLOSED.
* **New SESSION_OPEN** is accepted only in NONE/CLOSED, after all workers have reported DETACHED or are gone. A
  worker that dies during DETACH counts as gone; its replacement starts detached. Otherwise the reply is
  `SESSION_REJECT(busy)`.
* **Races:**
  * EOF together with SESSION_CLOSE: the first one wins; the second is ignored.
  * ECHO timeout, then EOF: UNRESPONSIVE → CLOSING.
  * FENCE, then EOF before the pidfd reports: CLOSING.
  * New SESSION_OPEN during FENCING/CLOSING: busy.
  * DETACH arriving at a worker already DETACHING after data-link EOF: idempotent per session_id.

### 9.2 Two independent health mechanisms (D-S12)

| | Supervisor ↔ worker ECHO (2a-6) | BSC ↔ Supervisor session ECHO |
|---|---|---|
| Parameters | `echo-interval` 2 s, `echo-misses` 3 (APPROVED defaults, plan §11, item 1) | `session-echo-interval`, `session-echo-misses`: keys PROPOSED, values **not decided**, to be tuned in S4/S7 (UNRESOLVED, not blocking S1–S3) |
| First miss | that worker is *suspect*: WORKER_STATE to the BSC → ineligible for new connections (plan §11, item 1) | session UNRESPONSIVE (logged, status) |
| Threshold reached | that worker is lost (`worker_lost("liveness")`) → collection → restart policy / FAILED; WORKER_DOWN to the BSC | **no action on workers or the session**; stays UNRESPONSIVE until echoes return (→ ACTIVE), EOF (→ CLOSING) or operator fencing |
| Effect on the other mechanism | none: a worker failure never affects the session or other workers | none: never classified as a worker failure; workers keep running and keep their data links |

* **BSC unresponsive, Supervisor and workers healthy:** the session stays UNRESPONSIVE. Workers stay attached, and
  their data links are untouched (the BSC owns the other ends). A new BSC is refused (busy). Recovery is the BSC
  answering again, its death (EOF), or operator fencing.
* **One worker unresponsive, session healthy:** only that worker becomes suspect and then is lost and restarted.
  The BSC gets WORKER_STATE/WORKER_DOWN, then WORKER_UP for the replacement. The other workers and the session are
  unaffected.
* **BSC side:** the BSC also echoes the Supervisor. An unresponsive Supervisor is logged and alarmed only; the data
  links go directly to the workers and keep working. Supervisor death is seen as EOF on the session and, because the
  workers die with it, as EOF on their data links. The BSC marks those workers lost and reconnects with backoff.

### 9.3 Fencing

Fencing is operator-forced and only on the Supervisor (operator role, `FENCE_SESSION`):
1. The peer identity is taken at SESSION_OPEN: `SO_PEERCRED` and a pidfd (`pidfd_open()`) [T: Linux ≥ 5.3 and
   permission; if unavailable, fencing is refused].
2. FENCING: `pidfd_send_signal(SIGKILL)`, then wait until the pidfd reports exit, bounded.
3. Exit confirmed, or EOF → CLOSING.
4. Not confirmed → FENCE_FAILED. Nothing is admitted.

ECHO timeout alone never admits a second BSC. Preferred procedure: stop the old BSC by ordinary means first; its
death closes the session (EOF).

The old BSC cannot act as an independent pool owner:
* In supervisor mode it has no spawning code path, and its view contains no transport commands (§4.2).
* A BSC that receives SESSION_REJECT exits (§4.6).
* A fenced BSC is dead.
* A second BSC on the same host and address cannot bind the Abis ports while the old one holds them [T]. This is a
  secondary safeguard only.

### 9.3a As implemented (S2-5; `aworker_session.c`, `aworker_session.h`)

* The state machine of §9.1 (NONE, ACTIVE, UNRESPONSIVE, FENCING, CLOSING, CLOSED), one session at a time, session ids
  never reused within a boot. Admission refusals: busy, version, bad-document, config-mismatch, no-sessions.
* Attaching: per READY worker a socketpair; ATTACH (with one end) to the worker, WORKER_UP (with the other, and the
  worker's cs7 entries) to the BSC; no copy kept. ATTACHED (WORKER_STATE to the BSC) once the worker's STATE(attach-ok)
  arrived and WORKER_UP was written. A lost worker: WORKER_DOWN(lost); its replacement is attached again. A worker
  that detached itself (EOF or error on its data link): WORKER_DOWN(detached), attached again after 1 s.
* CLOSING's entry action exactly once: ECHO stopped, the connection closed, DETACH to each attached worker, the id
  retired; CLOSED once every worker detached or is gone.
* Session ECHO (D-S12 2 s / 3): UNRESPONSIVE from the first unanswered ECHO; the threshold is logged; no action.
* Fencing: SIGKILL through the pidfd taken at admission, confirmation within 3 s, else FENCE_FAILED (still FENCING,
  nothing admitted; FENCE_SESSION may be repeated). Without a pidfd fencing is refused.
* The worker side (aworker.h): ATTACH only while DETACHED and for its own incarnation; the data link's HELLO asserts
  boot nonce, incarnation and session; minimal detach unbinds every SCCP user and closes the data link.

### 9.4 Socket security (N5, decided 2026-11-18; implemented in S2-5)

**Decision and implementation:** the socket exists only with `supervisor-socket PATH` (absolute; no abstract
namespace). Its directory must exist (never created), belong to the Supervisor's effective uid and not be writable by
group or others. `PATH.lock` (0600, this user's, created if missing, never removed) is held with an exclusive
non-blocking `flock` for the Supervisor's lifetime and taken before any worker starts: a second Supervisor exits. A
leftover socket at PATH is removed only while holding the lock; anything else at PATH is refused. The socket is bound
with umask 0177 and set to 0600, and removed at a regular stop. Peers -- the BSC and the operator alike -- must have the
Supervisor's effective uid or be root (`SO_PEERCRED`, checked before anything is read; anything else is closed at
once and logged). Tested in `tests/aworker_session` X1, X2, X15.

The requirements as stated before the decision:

N5 must define, and S2-5 must test:
* ownership and mode of the socket directory and of the socket file;
* peer authorization by `SO_PEERCRED` (uid/gid policy), separately for the session role (one BSC) and the operator
  role;
* stale socket handling: a socket file is removed only after this Supervisor holds the lock, never while another
  Supervisor holds it;
* lock semantics: an exclusive, non-blocking `flock` on a lock file, held for the Supervisor's lifetime, and its path;
* rejection of unauthorized local clients before any message is processed;
* whether abstract-namespace sockets are excluded. They have no filesystem permissions, so excluding them is the
  expected answer.

umask alone is not a sufficient policy.

## 10. Detached worker (D-S9)

### 10.1 States and sequence [A]

ATTACHED(session) → DETACHING → DETACHED.

* DETACH or data-link EOF/error triggers DETACHING (idempotent).
* ATTACH is accepted only in DETACHED.
* The control link stays up throughout.

The DETACHING sequence:
1. Stop accepting.
   * A new N-CONNECT.ind (an MSC CR) is answered at once with `osmo_sccp_tx_disconn()` (CREF).
   * Connectionless SCU_UP (paging, RESET, RESET ACK) is dropped and counted.
2. Request release of each tracked connection: executed TX_CONN_REQ/TX_CONN_RESP and delivered N-CONNECT.ind, minus
   those ended. Each one still existing (`osmo_sccp_conn_id_exists()` [V]) gets `osmo_sccp_tx_disconn()`; the return
   value is recorded.
3. Confirmation window: `detach-release-timeout`, **value not decided**.
4. Unbind all SSN 254 users (`osmo_sccp_user_unbind()`).
5. DETACHED: SS7, ASP/AS and SCTP unchanged. Counters are reported in STATE.

### 10.2 Source findings [V] and what they do not prove

* **What `osmo_sccp_tx_disconn()` returns.** It calls `osmo_sccp_tx_disconn_data()` (`sccp_helpers.c`), which
  returns `-ENOTCONN` for an unknown connection and otherwise the result of `osmo_sccp_user_sap_down()`. That only
  says whether SCOC **accepted** the N-DISCONNECT.req.
* **ACTIVE** (`sccp_scoc_fsm.c` `scoc_fsm_active()`): RLSD is generated via `sccp_conn_xua_gen_encode_and_send()`,
  which **ignores** the return of `sccp_scrc_rx_scoc_conn_msg()`. Then T(rel) starts and the state is DISCONN_PEND.
* **CONN_PEND_OUT** (`scoc_fsm_conn_pend_out()`): the cause is recorded and the state moves to WAIT_CONN_CONF.
  **No RLSD is sent at that time.**
* **CONN_PEND_IN** (`scoc_fsm_conn_pend_in()`, N-DISCONNECT.req): CREF is sent and the connection goes to IDLE ("we've ignored CREF sending
  errors").
* **Transmit path:** `scrc_node_12()` → `gen_mtp_transfer_req_xua()` → `sua2sccp_tx_m3ua()` → … →
  `osmo_ss7_asp_send()` (`ss7_asp.c`). Success means **enqueued** on the stream; it returns `-EIO` without a
  connected stream. The leg from `sua2sccp_tx_m3ua()` to `osmo_ss7_asp_send()`, including AS selection and any
  AS-PENDING queueing, is **not traced** [T].
* **DISCONN_PEND** (`scoc_fsm_disconn_pend()`):
  * a peer RLC or RLSD frees the connection;
  * T(rel) resends RLSD and starts T(int) and T(repeat_rel);
  * T(repeat_rel) resends RLSD;
  * T(int) gives up and frees;
  * a routing failure is ignored;
  * defaults: T(rel) 10 s, T(repeat_rel) 10 s, T(int) 60 s (`sccp_instance.c`).
* **Unbind frees remaining connections silently:** `osmo_sccp_user_unbind()` → `sccp_user_free()` →
  `sccp_user_flush_connections()` → `sccp_conn_free()`. Timers stop and the FSM terminates; nothing is sent.

Four different things must never be conflated:

| | Meaning | Evidence available |
|---|---|---|
| API acceptance | `osmo_sccp_tx_disconn()` returned 0 | return value |
| Wire transmission | the RLSD left the host | only externally observable (test peer, capture) [T] |
| Peer response | an RLC/RLSD arrived | the connection disappears from the instance before T(int) [V `scoc_fsm_disconn_pend()`] |
| Local removal | the connection object is gone | `osmo_sccp_conn_id_exists()` false; also true after T(int) or unbind, so it is **not** evidence of a peer response on its own |

Reported counters:

| Counter | Meaning |
|---|---|
| `confirmed` | removed within the window, which is shorter than T(int) |
| `unconfirmed` | still present at unbind |
| `not_issued` | API error |
| `pending_conn_conf` | CONN_PEND_OUT/WAIT_CONN_CONF; no RLSD sent |
| `refused_incoming` | CONN_PEND_IN, CREF sent |
| `not_issued_transport_down` | AS/ASP inactive; how this is detected is [T] |

### 10.3 SSN 254 after unbind [V]

| Incoming | Path | Result |
|---|---|---|
| CR | `scrc_node_6()` → `scrc_node_4(UNEQUIPPED_USER)` → `sccp_rout_fail_enqueue()` → `rout_fail_pending_cb()` → `sccp_scoc_rx_scrc_rout_fail()` → `gen_coref_without_conn()` | CREF "unequipped user" |
| UDT | → `sccp_sclc_rx_scrc_rout_fail()` | UDTS "unequipped user", sent regardless of return option ("TODO") |
| DT1 / IT / RLSD for an unknown reference | `sccp_scoc_rx_unass_local_ref()` | DT1 discarded; IT/CC/RSR/RSC → ERR; RLSD → RLC |
| SST | `scmg_rx_sst()`: no user | no answer |
| (unbind itself) | `sccp_scmg_tx()` is called only for the SSA reply | no SSP |

Bound without a BSC: CR and UDT reach `worker_sap_up()`, which drops them, and SST gets SSA. This is **rejected**
(false availability).

osmo-msc: `handle_notice_ind()` → `RAN_PEER_EV_UNAVAILABLE` → `ran_peer_discard_all_conns()` + WAIT_RX_RESET [V].
Other vendors: [T].

### 10.4 Timeout determination: UNRESOLVED/BLOCKING for S5 sign-off

There is no production default until the plan §13.4 protocol tests have produced evidence. The pool refuses to
start in supervisor mode if no value is configured, until a default is approved [A].

### 10.5 Reattach

After the unbind the worker's user table is empty (as `worker_cleanup()`/`user_free()` today [V]). The new BSC binds
with fresh `user_ref`s through `handle_user_bind()` [V]. BIND_RESULT → `a_worker_available()` →
`bssmap_reset_resend_reset()` [V] → RESET.

## 11. BSSMAP RESET [V]

* BSC start: `osmo_bsc_sigtran_init()` → `a_reset_alloc()` → `bssmap_reset_alloc()` enters DISC after 1 ms.
  `bssmap_reset_fsm_timer_cb()` → `tx_reset()` repeats every T4 until RESET ACK.
* In pool mode, `a_worker_available()` also calls `bssmap_reset_resend_reset()`.
* A restarted BSC has an empty conn_id registry.
* osmo-msc: `ran_peer_rx_reset()` → `ran_peer_discard_all_conns()` → RESET ACK. From WAIT_RX_RESET only a RESET
  restores READY.
* **RESET is not suppressed.** It travels over the same association, so transport continuity is unaffected.

## 12. Lifecycle sequences [A]

**Startup** (any order):
```
Supervisor: read osmo-bsc.cfg → view builder → global checks → canonical document → snapshots (memfd)
            → lock + listen on supervisor-socket → spawn workers (fork, fexecve self, worker role)
Worker i:   parse snapshot → cs7 X up (SCTP, M3UA) → HELLO, CS7_INFO on control → READY, DETACHED (SSN 254 unbound)
BSC:        read osmo-bsc.cfg → BSC view → global checks → canonical document → connect (retry with backoff)
            → SESSION_OPEN(version, pid, fd: canonical document) → SESSION_ACCEPT(session_id, workers)
            → per READY worker: ATTACH(session, fd) → WORKER_UP(fd) → data HELLO → USER_BIND → BIND_RESULT
            → BSSMAP RESET → RESET ACK → MSC usable
```

**Normal BSC shutdown:**
```
BSC: SESSION_CLOSE → exit (no GOODBYE; the legacy aworker_pool_stop()/atexit path is not used in supervisor mode)
Supervisor: CLOSING (once) → DETACH all → workers: DETACHING → DETACHED; PIDs, SCTP, M3UA unchanged
```

**BSC SIGKILL:**
```
kernel closes BSC fds → Supervisor: EOF → CLOSING (once) → DETACH all; workers: data EOF or DETACH → DETACHED
MSC: next UDT → UDTS "unequipped user" → osmo-msc: discard conns, WAIT_RX_RESET (osmo-msc [V]; others [T])
```

**BSC restart and reattach:** as in Startup from "BSC:": same worker PIDs and incarnations, a new session_id, new
data links; USER_BIND, RESET/RESET ACK.

**Worker failure and restart** (session attached):
```
worker exits/killed or ECHO lost → Supervisor: collect, policy (backoff / restart limit / FAILED: unchanged code)
→ WORKER_DOWN(idx, incarnation, reason) → BSC: that cs7's users unavailable, its RESET FSMs → DISC
→ after the delay: spawn (new incarnation) → READY → ATTACH(current session) → WORKER_UP → bind → RESET
other workers and the session: unaffected
```

**Supervisor failure:**
```
Supervisor dies → workers: PDEATHSIG SIGTERM + control EOF → exit; their associations close
BSC: session EOF + data EOF → all workers lost → reconnect with backoff
a new Supervisor (started by external tooling) → fresh pool → session → RESET
```

**Pool shutdown:** only through the Supervisor (SIGTERM, or operator POOL_SHUTDOWN). It runs the existing
`aworker_pool_stop()` path: GOODBYE flush, grace period, SIGTERM/SIGKILL, collection [V, 2a-1].

## 13. Supervisor failure scope (APPROVED DIRECTION)

* Workers keep `PR_SET_PDEATHSIG, SIGTERM` and the `getppid()` check against their parent, the Supervisor [V].
* Control-link EOF ends a worker (today's `w_closed_cb()`, kept for the control link). Data-link loss only detaches.
* A new Supervisor creates a fresh pool. Continuity is out of scope.

## 14. Source and build separation (S1), and preservation of Phase 1/2

**As implemented (S1, closed and accepted):** `src/a-worker/libaworker.la` holds `aworker.c`, `aworker_cfg_scan.c`,
`aworker_ipc.c`, `aworker_ipc_link.c`, `aworker_log.c` (the shared category table `osmo_bsc_categories[]` and the
log category variable), `aworker_pool.c` and `aworker_vty.c`; S2-1 and S2-2 added `aworker_cfg_view.c` and
`aworker_snapshot.c`. The BSC user glue is `src/osmo-bsc/aworker_users.c` in `libbsc.la`, which links
`libaworker.la`. Differences from the target layout of §14.2: the `show a-worker-pool` and `a-worker-pool reset
worker` VTY commands stayed in `libaworker.la` (`aworker_vty.c`), as N4 records. **S2-3:** the header split (N4) is
done -- the SCCP-user API is in `aworker_sccp_user.h`, and no `libaworker` object includes `bsc_sccp_backend.h` -- and
`src/a-worker/` builds the program `bsc-aworker` (`aworker_main.c`).
§14.1 below is the state **before** S1.

### 14.1 Build facts before S1 [V]

* `src/osmo-bsc/Makefile.am`: `noinst_LTLIBRARIES = libbsc.la` (includes `aworker.c`, `aworker_cfg_scan.c`,
  `aworker_ipc.c`, `aworker_ipc_link.c`, `aworker_pool.c`, `aworker_vty.c`); `bin_PROGRAMS = osmo-bsc` =
  `osmo_bsc_main.c` + `libbsc.la`.
* `src/Makefile.am`: `SUBDIRS = osmo-bsc utils [ipaccess]`.
* `configure.ac`: the `PKG_CHECK_MODULES` used are libosmocore, libosmovty, libosmoctrl, libosmogsm (≥ 1.14.2),
  libosmoabis, libosmo-netif, libosmo-sigtran (≥ 2.3.0) and libosmo-mgcp-client.
* BSC couplings in the A-worker code:
  * `aworker_pool.c` includes `bsc_sccp_backend.h` and implements the `bsc_sccp_user` ops;
  * `aworker.c` uses `tall_bsc_ctx` and includes `gsm_data.h`;
  * `aworker_vty.c` uses `tall_bsc_ctx` and `AWORKER_POOL_NODE` (enum in `include/osmocom/bsc/vty.h`);
  * the log category `DAWORKER` comes from `include/osmocom/bsc/debug.h`, and the category table is in
    `osmo_bsc_main.c`.
* `aworker_ipc.c`, `aworker_ipc_link.c` and `aworker_cfg_scan.c` take the log category as a parameter or use none.

### 14.2 Target layout

| Unit | Content |
|---|---|
| `src/a-worker/libaworker.la` (noinst) | `aworker_ipc.c`, `aworker_ipc_link.c`, `aworker_cfg_scan.c` (moved as is); `aworker_vty.c` config DEFUNs and writer (shared syntax); `aworker.c` (worker role); **lifecycle part of `aworker_pool.c`** (below); the shared log category table (data only, moved from `osmo_bsc_main.c`) |
| `src/a-worker/bsc-aworker` (bin, created in S2-3) | `aworker_main.c`; links `libaworker.la`, libosmocore, libosmovty, libosmo-netif, libosmo-sigtran (libosmogsm only if a moved file needs it [T]) |
| `src/osmo-bsc/libbsc.la` | everything else, plus the **user part of `aworker_pool.c`** (below) in its own file, plus BSC-side `show`/`reset` VTY commands; links `../a-worker/libaworker.la` |
| `src/Makefile.am` | `SUBDIRS = a-worker osmo-bsc utils …` |
| `configure.ac` | `src/a-worker/Makefile` in `AC_OUTPUT` |
| tests | unchanged sources; Makefiles add `libaworker.la` |

### 14.3 Function-level split of `aworker_pool.c` [V function list]

* **Lifecycle** (→ `libaworker.la`):
  * failure records and config: `aworker_fail_reason_name`, `aworker_pool_cfg`, `aworker_pool_enabled`,
    `aworker_pool_prescan_config`, `ownership_build`, `aworker_pool_check_ownership`, `ownership_log`,
    `aworker_pool_expected_owner`;
  * restart policy: `ms_since`, `log_exit_status`, `worker_fail`, `restart_delay_for`, `worker_is_stable`,
    `worker_next_step`, `restart_in_window`, `restarts_in_window`, `restarts_expire`, `worker_schedule_restart`,
    `worker_retry_start`, `worker_apply_policy`;
  * collection and loss: `worker_collected`, `worker_try_collect`, `reap_timer_cb`, `worker_begin_termination`,
    `restart_timer_cb`, `worker_lost`;
  * link handling: `link_ready_cb`, `ownership_claim_ok`, `handle_cs7_info`, `link_rx_cb` (dispatch),
    `link_closed_cb`, `link_create`, `start_timer_cb`;
  * spawning: `is_log_opt`, `fd_above_stdio`, `aworker_pool_set_exec`, `is_kept`, `sys_close_range`,
    `close_fds_by_rlimit`, `close_fds_by_proc`, `child_close_fds`, `child_exec_worker`, `worker_spawn`;
  * pool and stop: `startup_timer_cb`, `pool_alloc`, `aworker_pool_start`, `aworker_pool_start_with_fds`,
    `queue_goodbye`, `goodbye_written`, `goodbye_pending`, `stop_wait`, `stop_processes`, `aworker_pool_stop`;
  * queries: `aworker_pool_shutdown_was_fatal`, `aworker_pool_num_workers`, `aworker_pool_worker_info`,
    `aworker_pool_reset_worker`, `aworker_pool_all_ready`, `aworker_pool_cs7_info`;
  * the worker part of `aworker_pool_dump_vty`.
* **BSC user glue** (→ `libbsc.la`):
  * lookups: `worker_by_cs7`, `cs7_entry`, `user_by_ref`, `user_by_cs7_ssn`;
  * binding: `user_set_unavailable`, `user_try_bind`, `users_try_bind_all`, `users_lose_worker`,
    `handle_bind_result`;
  * receive: `handle_scu_up`, `handle_tx_failed`;
  * transmit: `user_send`, `tx_user`, `check_len`, `w_tx_unitdata_msg`, `w_tx_conn_req_msg`, `w_tx_conn_resp`,
    `w_tx_data_msg`, `w_tx_disconn`;
  * API: `aworker_sccp_user_bind`, `_find`, `_unbind`, `_ready`;
  * the user part of `aworker_pool_dump_vty`.
* **BSC-only checks:** `aworker_pool_check_main_cs7` inspects the BSC's own cs7 instances, so it stays in libbsc.
* **The seam:** the existing direct calls from lifecycle into user glue become a small callback table registered by
  libbsc, with **the same call sites and order**:
  * worker ready (`users_try_bind_all`);
  * worker lost (`users_lose_worker`);
  * user message received (BIND_RESULT / SCU_UP / TX_FAILED);
  * startup diagnostic (unbound users);
  * status dump.
* **Decoupling:** `tall_bsc_ctx` becomes a context parameter. `DAWORKER` becomes a log category variable set by the
  host program. `AWORKER_POOL_NODE` comes from a header (no symbol). `gsm_data.h` is dropped from `aworker.c`.
* **Behaviour must not change.** S1 adds no fault-tolerance behaviour. Its gates (plan §13.3) require byte-identical
  P1–P20/T1–T11 output, legacy E2E 46/46, and a `git diff -M` that shows only moves and the seam.

### 14.4 Phase 1/2 guarantees

| Feature | Owner in supervisor mode | Change |
|---|---|---|
| Phase 1 spawn/exec/reap, PDEATHSIG, fd hygiene, non-blocking collection | Supervisor (`libaworker.la`) | moved verbatim |
| 2a-1 GOODBYE | Supervisor (pool shutdown) | a BSC stop becomes SESSION_CLOSE |
| 2a-2 ownership + global checks | Supervisor and BSC (shared) | input: inline form + view builder |
| 2a-3a FAILED + reset | Supervisor; reset forwarded by the BSC or requested by the operator | none |
| 2a-3b runtime CS7_INFO check | Supervisor | kept; purpose in snapshot mode per §4.4 |
| 2a-4 backoff, limit, Policy B | Supervisor | none |
| 2a-5 per-start startup timeout (default 60 s) | Supervisor | none |
| SCCP users, TX path | BSC | moved verbatim |

**Tests:**
* P1–P20 and T1–T11 keep sources and meaning. The test process plays the parent, which in supervisor mode is the
  Supervisor's role.
* P2/P3 keep asserting death with the parent: **not inverted**.
* New S-series tests cover the executable, sessions and detached mode.

Approved decisions stand:
* one worker per cs7 instance / A link with its own BSC point code;
* several links per MSC;
* no migration of existing SCCP connections;
* new connections only to eligible workers (ASP ACTIVE, MSC PC reachable, RESET ACK);
* worker-local failures;
* global errors refuse startup.

Approved defaults are unchanged. **2a-6 does not resume before the S7 gate** (plan §13.6).

## 15. Responsibility boundary and handoff

**Out of scope:** systemd units, service dependencies and ordering, restart policies, cgroups, deployment scripts,
production service management. Nothing in the design uses systemd APIs.

Handoff requirements for DevOps:
* Two binaries. Both read the same `osmo-bsc.cfg`: `bsc-aworker -c FILE` and `osmo-bsc -c FILE`. **As of S2-3** only
  `bsc-aworker` runs the Supervisor; osmo-bsc cannot attach to it yet (S2-7), so do not run both with the same inline
  pool (see the status note at the top).
* The Supervisor must run independently of the BSC's lifetime. It must not be the BSC's child, and stopping or
  restarting the BSC must not signal the Supervisor or its process group.
* Stopping the Supervisor stops the pool and the transport.
* The directory of `supervisor-socket` must be writable by the Supervisor. The socket's permissions control who may
  attach as BSC or operator. (S2-5; not implemented yet.)
* Fencing needs Linux ≥ 5.3 (pidfd) and permission to signal the BSC; otherwise fencing is refused.
* Logging per §7. A shared log file was tested in the default (`nonblocking-io`) and `blocking-io` modes only; do not
  use `wq` for a shared file until it is tested.
* Operator CLI: `bsc-aworker --control PATH …` (§5.2; S2-5, not implemented yet). Until then: signals only
  (SIGTERM / SIGINT stop the pool; SIGUSR1 prints a talloc report).

## 16. Decision table D-S1 … D-S12

| # | Decision | Status | Resolution |
|---|---|---|---|
| D-S1 | Inline worker cs7 configuration; legacy compatibility | **APPROVED** (S2-0 review); grammar VERIFIED unambiguous; **implemented** in S2-1 (deprecation notice not yet) | `worker I cs7-instance X`; one form per worker and per pool, mixing is a global error; supervisor mode inline only; file form deprecated, legacy mode only; no implicit precedence (§4.5) |
| D-S2 | Supervisor snapshot | **APPROVED** (S2-0 review): sealed memfd with a tested unlinked-file fallback; `vty_read_config_filep()` VERIFIED; **implemented for legacy mode** in S2-2 (§4.4) | Views built once at start; sealed memfd per worker; workers never read `osmo-bsc.cfg`; 2a-3b check kept with its purpose restated; legacy file form unchanged (§4.4) |
| D-S3 | Mismatch at reattach | APPROVED DIRECTION; detail PROPOSED | Compared: H + P (effective, explicit defaults) + C (syntactic cs7 subtrees). The BSC builds C without parsing asp nodes. Mismatch → `SESSION_REJECT(config-mismatch)` → BSC exits 1 (startup convention) (§4.6) |
| D-S4 | Digest scope and canonicalisation | APPROVED DIRECTION; specification PROPOSED; **C is syntactic by necessity** (VERIFIED constraint) | Scope and rules in §4.6; BSC-only keys excluded; versioned header; false mismatches possible, false matches not |
| D-S5 | Comparison method | **APPROVED as direction** (S2-0 review); contents decided 2026-11-18 (CD-1); implemented in S2-5 | Deterministic canonical-document comparison, not hash-only; exact contents defined by CD-1 before S2-5 (§4.6) |
| D-S6 | Supervisor operational interface | **APPROVED** (S2-0 review): no runtime management VTY server; configuration command registration stays; **implemented** in S2-3 (no socket in the Supervisor) | §5.2. The operator control channel is part of S2-5 and depends on N5 |
| D-S7 | Supervisor port | VERIFIED: none assigned | Not needed for the MVP; deferred to deployment / port registry; nothing reserved (§5.3) |
| D-S8 | `on-worker-failure shutdown` | APPROVED DIRECTION; VERIFIED feasible; **implemented** in S2-3 | Global configuration error in supervisor mode; legacy unchanged (§6) |
| D-S9 | Detach release timeout | **UNRESOLVED/BLOCKING** for S5 sign-off (not S1–S4) | No default; set from plan §13.4 tests; pool refuses supervisor mode without a value until approved (§10.4) |
| D-S10 | Logging | **APPROVED with limitations** (B2, 2026-11-17); worker part implemented in S2-2, Supervisor part, identity (B5) and the durable test in S2-3 | Workers: no log file, stderr inherited (D-2); Supervisor keeps its targets; BSC unchanged; shared file tested for default and `blocking-io` only, `wq` unverified; durable regression test required in S2-3 (§7) |
| D-S11 | Supersede the previous architecture | APPROVED DIRECTION | Documents updated; `PHASE2_ARCHITECTURE.md` keeps history with explicit supersession notes |
| D-S12 | ECHO separation | APPROVED DIRECTION; worker defaults APPROVED; session ECHO keys and initial values decided 2026-11-18 (2 s / 3); implemented in S2-5 | Two independent mechanisms; state transitions and effects in §9.2 |
| N2 | Inline form in legacy mode as a stepping stone | **APPROVED** (S2-0 review) | S2-2 (plan §14.3) |
| N3 | Legacy worker argv contract unchanged; separate contract for the standalone executable | **APPROVED** (S2-0 review); kept and verified in S2-2 (P1–P20 output unchanged, legacy E2E 46/46) | §3 |
| B3 | Executable name | **APPROVED** (final) | `bsc-aworker` everywhere; runtime names `bsc-aworker-<i>` (memfd), `bsc-aworker-snap.<random>` (fallback directory) |
| B4 | Worker-role arguments | **APPROVED** | `--aworker-fd`, `--aworker-snapshot-fd` kept; internal, strictly validated; legacy `--aworker-config FILE` unchanged (§3) |
| B5 | Logging identity prefixes | **APPROVED** | new executable only; legacy osmo-bsc output unchanged (§7) |
| N4 | Split lifecycle and BSC-specific public headers in S2-3; no other mechanical moves | **APPROVED** (S2-0 review); **done** in S2-3 | `aworker_sccp_user.h` (§14) |
| N5 | Session and operator socket security | **DECIDED** 2026-11-18; implemented in S2-5 | §9.4 |
| CD-1 | Exact contents of the canonical document | **DECIDED** 2026-11-18 (H/P/C as proposed); implemented in S2-5 | §4.6, plan §14.4 |

## 17. Risk register

| Risk | Impact | Validation / gate | Blocking? |
|---|---|---|---|
| RLSD not on the wire before the unbind (untraced AS leg) | stale MSC connections until UDTS or RESET | §13.4 tests (S5) | blocks D-S9 |
| No RLSD for CONN_PEND_OUT at detach | as above | same | blocks D-S9 |
| Detecting AS/ASP active in the worker | classification only | S5 | no |
| Vendor MSC UDTS/CREF handling | traffic lost until RESET on that vendor | per-vendor test (D6) | blocks vendor claims only |
| View builder vs parser divergence | ASP in the wrong process; wrong worker view | §4.3 golden tests; prescan re-check; post-parse AS check; since S2-2 also the worker's D-13 text check and post-parse ownership check | S2-1 gate passed; S2-2 checks in place |
| C syntactic, not effective | false mismatch → refusal (safe) | documented; optional effective worker digest [T] | no |
| memfd / seals availability | fallback path | both paths tested in S2-2 (unit S-1…S-10, process P21–P29, real binary R-5/R-7); fallback weaker (same user) | no |
| fd passing details (cmsg size, `MSG_CTRUNC`, no `MSG_CMSG_CLOEXEC`, deferred send) | descriptor leak or misuse | S4 unit tests incl. failure injection | blocks S4 sign-off |
| pidfd availability and permissions | fencing refused (safe) | S4 tests of both outcomes | no |
| Shared log file line atomicity | interleaved lines | default and `blocking-io`: experiment and the durable test `tests/aworker_logfile` (S2-3) pass; `wq` untested | no |
| Category table move changing BSC logging | regression | S1 byte-identical gates | blocks S1 sign-off |
| Splitting `aworker_pool.c` / decoupling `aworker.c` | regression | S1 gates | blocks S1 sign-off |
| Session ECHO values | false UNRESPONSIVE alarms | tuning in S4/S7 | no (alarm only) |
| Abis bind failure for a second BSC | secondary safeguard only | process test | no |

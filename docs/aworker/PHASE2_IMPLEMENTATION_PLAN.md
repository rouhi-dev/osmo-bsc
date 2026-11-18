# A-worker pool, Phase 2: implementation plan, task breakdown, acceptance criteria

Status: **approved** (with the clarifications in §11); implementation in progress. Architecture: `PHASE2_ARCHITECTURE.md` (approved with the decisions
below). Baseline: `a-worker-pool` at `e54578f2c` (Phase 1).

> **Revision (after 2a-5, HEAD `fa01448ea`): implementation is paused.** The target architecture now includes a
> standalone Supervisor executable `bsc-aworker` (`src/a-worker/`) so that workers survive a BSC crash. The
> single configuration source is `osmo-bsc.cfg`. See `PHASE2_SUPERVISOR.md` (revision 3, current target, for final review). The migration
> plan is §13 below. 2a-6 onwards resumes only under the conditions in §13.6. §1–§12 remain valid for what they
> describe; where they say "the main process" owns workers, supervisor mode replaces that with the Supervisor.
> S1 is closed and accepted (uncommitted on `fa01448ea`). The S2-0 review (§14) records the approved decisions, the
> implementation subphases S2-1 … S2-8 and their mapping to the phases of §13.
>
> **Status (2026-11-18):** S1, S2-1 and S2-2 are closed and accepted (S2-2 record: §14.5). S2-3, the standalone
> Supervisor `bsc-aworker`, is implemented and verified (§14.6), and so is S2-4, IPC protocol 2.0 with descriptor
> passing (§14.7); S2-5 … S2-8 have not started. The executable name is `bsc-aworker` (decision B3).

## 0. Approved decisions this plan implements

| # | Decision | Where |
|---|---|---|
| D1 | Calls/connections of a failed worker may be lost; isolate, keep other links serving, admit new work on OPERATIONAL links | 2b, 2c |
| D2 | `get_msc_by_addr()` must match (cs7 instance, address); compatible with existing configs; prepare an upstream contribution | 2c-1, §7 |
| D3 | Worker-specific config error → that worker FAILED with a reason; BSC and other workers continue; global errors stay global; all workers failed → BSC alive, A interface reported unavailable | 2a, §2 |
| D4 | GOODBYE flush and clean shutdown belong to 2a | 2a |
| D5 | No state replication, export/import API or STP change in Phase 2 | — |
| D6 | Several BSC point codes per MSC = explicit interoperability requirement, validated per target vendor before claiming production compatibility | 2e, §8 |
| R1 | A failed or non-OPERATIONAL link is ineligible **immediately** | 2b |
| R2 | Remaining OPERATIONAL links keep accepting new connections without waiting for replacements | 2b, 2c |
| R3 | A replacement is eligible only after ASP/AS ACTIVE, MSC point-code reachability and BSSMAP RESET-ACK | 2b |
| R4 | Selection uses explicit per-link counters and admission limits | 2c |
| R5 | Phases 2a–2e, independent tests, reviewable commits; Phase 1 behaviour and tests preserved | all |

## 1. Ground rules for every commit

* Builds without new warnings. `make check` passes in the **normal** and the **ASan+UBSan** build (today 14/14).
  Phase 1 tests (`aworker_pool` T1–T9, `aworker_proc` P1–P4) stay green and unchanged, unless a commit deliberately
  changes a documented behaviour; then the commit says so and updates the expectation.
* One logical change per commit. Each sub-phase ends with an end-to-end run (the Phase 1 harness, extended).
* No push and no change to `~/dev/gsm`, its submodules or `_install`. Work in the worktree; build out-of-tree.
* For every new test, at least one deliberate mutation of the fix must make it fail (as done in Phase 1).

## 2. Global vs. worker-specific configuration errors (D3)

| Error | Class | Behaviour |
|---|---|---|
| `asp`/`as`/`listen` in the main file (`aworker_pool_prescan_config`) | **global** | refuse to start (unchanged) |
| `workers N` but `worker i config-file` missing (`aworker_pool_start`) | **global** | refuse to start (unchanged) |
| Two worker files claim the same cs7 instance (detected by the new start-time scan) | **global**: the pool config contradicts itself | refuse to start, naming both files and the instance |
| An `msc` whose cs7 instance no worker file owns | **global** | refuse to start (today it only logs after the startup timeout) |
| `--daemonize`, SCCPlite, SUA, Lb with pool mode | **global** | refuse to start (unchanged) |
| Worker file unreadable / unparsable at start or later | **worker** | that worker FAILED (reason: `config`), its links unavailable |
| Worker reports a cs7 set different from the expected map (file changed at runtime) | **worker** | FAILED (reason: `ownership`) |
| Worker point code ≠ main-file point code of that instance | **worker** | FAILED (reason: `point-code`); today only logged |
| Worker exits 66 (`AWORKER_EXIT_CONFIG`) | **worker** | FAILED (reason from the worker's last log line is not available; record exit 66 + time) |
| `execv`/`fexecve` failure in the child | **worker** | FAILED (reason: `exec`) |

"All workers FAILED" → osmo-bsc keeps running (Abis, VTY, CTRL). Every `msc` is unusable, so Complete Layer 3 is
rejected through the existing "No suitable MSC" path (`gsm_08_08.c` :485-488). The pool reports
`A interface: unavailable` in VTY and CTRL. No exit. `on-worker-failure shutdown` remains as an explicit opt-in that
overrides this.

## 3. Phase 2a: lifecycle completion

| Task | Change (files / functions) |
|---|---|
| 2a-1 GOODBYE flush | `aworker_ipc_link.c`: new `aworker_ipc_link_shutdown()`: stop accepting new records, let osmo_io drain the queue, then close and report (callback or bounded wait). `aworker_pool_stop` :1302 sends GOODBYE, drains each link for a bounded time, then the existing collection (Phase 1 `stop_processes`). Workers exit 0. |
| 2a-2 Expected ownership map + global checks | `aworker_pool_start` :1165: scan every worker file with `aworker_cfg_scan` (exists), build cs7→worker map; global errors per §2. `handle_cs7_info` :605: a claim outside the map → worker FAILED (`ownership`), replacing first-come. |
| 2a-3 Worker-local FAILED | `enum aworker_state` gets `AWORKER_ST_FAILED` with `fail_reason` and `failed_at`. `worker_apply_policy` :399: config error → FAILED, not `osmo_select_shutdown_request`. VTY `a-worker-pool reset worker <0-15>` (enable node) leaves FAILED. |
| 2a-4 Restart backoff + limit | Replace the fixed `restart_timer` delay with `delay = min(base·2^n, max)`, n reset after `stable-time` in READY/OPERATIONAL. `max-restarts N window S` → FAILED (`restart-limit`). Config: `restart-delay <min>` stays (= base, compatible); new `restart-delay-max`, `max-restarts`. |
| 2a-5 Startup timeout per start | Arm a per-worker timer in `worker_spawn` :1034 instead of only in `aworker_pool_start`; `startup_timer_cb` :1091 becomes per worker. |
| 2a-6 ECHO liveness | Pool sends `ECHO_REQ` (token = counter) every `echo-interval` to every READY worker; `link_rx_cb` accepts `ECHO_RSP` (today it closes the link on it); first missed echo → worker *suspect* (ineligible, see 2b), `echo-misses` consecutive → `worker_lost("liveness")`. |
| 2a-7 Deferred teardown | `link_closed_cb` :748 / transmit failures schedule `worker_lost` on a zero-delay timer, so teardown never runs inside a caller's FSM action. |
| 2a-8 Status | `aworker_pool_worker_info` gains `fail_reason`, `failed_at`, `restarts_in_window`, `last_echo_ok`. `show a-worker-pool` prints them and the pool-level A interface state (available / degraded / unavailable). |

**Tests**
* Unit (`aworker_pool_test`, scripted worker, `osmo_gettimeofday_override` fake clock):
  * echo sent, response accepted, misses → lost;
  * backoff sequence and reset after stable time;
  * limit → FAILED;
  * foreign claim → FAILED;
  * FAILED reset via VTY;
  * deferred teardown not re-entrant.
* Process (`aworker_proc_test`, real fork/exec): P5 SIGSTOP worker → killed after K·T and replaced; P6 bad worker file
  → FAILED, other worker READY, BSC (test process) alive; P7 crash loop → exponential delays, FAILED after N;
  P8 stop → workers exit **0** (GOODBYE received); P9 all workers FAILED → pool reports unavailable, process alive.
* Config scan (`aworker_cfg_scan_test`): duplicate claim across files, msc instance without owner.

**Acceptance (2a)**
1. A hung worker (SIGSTOP) is ineligible after the first missed echo (≤ 1× `echo-interval`). It is terminated after
   `echo-misses` and replaced, without blocking the event loop (gap < 400 ms, as P1).
2. A worker-specific config error puts only that worker in FAILED with a recorded reason. The BSC and other workers
   keep running.
3. A crash loop yields backoff delays base, 2·base, … ≤ max and then FAILED after `max-restarts` in `window`. No
   further spawns.
4. A clean stop → every worker exits **0** after GOODBYE. No zombies (P1 assertion kept).
5. Global errors in §2 refuse to start with a message naming the files and instances.
6. All Phase 1 tests are unchanged and green, normal and ASan.

**Commits (≈5):** link shutdown/GOODBYE · ownership map + global checks · FAILED + reset · backoff/limit + per-start
timeout · echo liveness + deferred teardown + status.

## 4. Phase 2b: protocol readiness and link eligibility

| Task | Change |
|---|---|
| 2b-1 `ASP_STATE` IPC | `aworker_ipc.h/.c`: new worker→main message `CS7_STATE {cs7_id, as_active, asp_active_count}` with codec, golden vectors and mutation coverage. `AWORKER_IPC_VERSION` 1 → 2 (same binary on both ends, Phase 1 fexecve). Worker (`aworker.c`): timer polls `osmo_ss7_as_active()` per owned instance (200 ms), sends on change and once after `CS7_INFO`. |
| 2b-2 Worker state machine | States `PROCESS_STARTING` (=STARTING), `PROCESS_READY` (=READY), `PROTOCOL_CONNECTING`, `OPERATIONAL`, `DEGRADED`, `DRAINING`, `TERMINATING`, `BACKOFF` (=RESTARTING), `FAILED`. Worker state = aggregate of its links. |
| 2b-3 Link readiness record per `msc` | `bsc_msc_data` (or a pool-side table keyed by msc nr): `as_active` (from CS7_STATE), `pc_inaccessible` (set by the existing N-PCSTATE handler `osmo_bsc_sigtran.c` :~310, cleared on "accessible" or on RESET-ACK), `reset_acked` (`bssmap_reset` CONNECTED). |
| 2b-4 RESET only on an active AS | `osmo_bsc_sigtran_tx_reset` :64 (the `bssmap_reset` `tx_reset` op): don't transmit while the link's AS isn't active, so the FSM's retry timer covers it. On the `as_active` false→true edge, call `bssmap_reset_resend_reset` (exists). |
| 2b-5 Eligibility predicate | `bool aworker_link_eligible(const struct bsc_msc_data *msc)` = worker ∈ {OPERATIONAL, DEGRADED} ∧ not suspect ∧ link `as_active` ∧ ¬`pc_inaccessible` ∧ `reset_acked` ∧ ¬draining. Pool mode off → always true (no behaviour change). Evaluated on demand, so every state change is immediately effective (R1). |
| 2b-6 Gate selection | `is_msc_usable` `gsm_08_08.c` :162: `&& aworker_link_eligible(msc)`. The paging-response fallback (:468) uses the same function. |

**Note [verified risk]:** SSNM is change-driven. If the MSC's PC was already available before our AS became active,
no DAVA may arrive. So "reachable" is proven by **RESET-ACK**, and N-PCSTATE only *removes* reachability. Behaviour
on activation must be checked in 2b's E2E run.

**Tests**
* IPC unit (`aworker_ipc_test`): CS7_STATE round trip, golden vector, boundaries, mutation sweep.
* Pool unit: link becomes eligible only after all three facts; each fact withdrawn → ineligible in the same event-loop
  iteration; DEGRADED not restarted; RESET suppressed until AS active.
* Process: P10 replacement does not send RESET before CS7_STATE(as_active).
* E2E: (a) no "no route" RESET for a replacement; (b) ASP blocked at the STP (`asp X block` / association drop on the
  STP side) → link DEGRADED, ineligible, worker **not** restarted, back to OPERATIONAL when unblocked; (c) MSC process
  stopped → link ineligible via DUNA/PCSTATE, other links unaffected.

**Acceptance (2b)**
1. A replacement is never eligible before AS active + no outstanding PC inaccessibility + RESET-ACK. The E2E log
   shows no RESET with "no route".
2. Any loss of those facts makes the link ineligible before the next selection call (unit test with a direct check
   after the event).
3. Link problems not caused by the worker give DEGRADED without restarts, and recovery without operator action.
4. Pool mode off: behaviour identical (existing BSC tests and VTY tests unchanged).

**Commits (≈4):** CS7_STATE IPC + version · worker state machine + link readiness · RESET gating · eligibility +
selection gate.

## 5. Phase 2c: several links per MSC, counters, admission control

| Task | Change |
|---|---|
| 2c-1 Lookup fix (D2) | `osmo_bsc_sigtran.c`: `get_msc_by_addr(uint32_t cs7_id, const struct osmo_sccp_addr *)` filtering `msc->a.cs7_instance` (as `get_msc_by_pc` :116). Callers :143 (`handle_unitdata_from_msc`, via `bsc_sccp_user_get_ss7(user)`), :161 (`handle_notice_ind`, `cs7` parameter), :191 (`handle_n_connect_from_msc`, via `user`). **Self-contained, first commit of 2c, written to apply to upstream master too.** |
| 2c-2 Per-link counters | Count live SCCP connections per `msc`: increment/decrement in `bsc_sccp_inst_register_gscon` / `unregister` (`bsc_sccp.c` :47/:75) via `gscon->sccp.msc`; stat item `msc:conn:active` and rate counters `msc:conn:assigned`, `msc:conn:rejected_capacity`. |
| 2c-3 Admission limits | `msc N / max-connections <0-65535>` (0 = unlimited, default; compatible). Over the limit → not eligible for new work. |
| 2c-4 Least-loaded selection | Factor the choice out of `bsc_find_msc` (`gsm_08_08.c` :177) into a pure function `msc_select(candidates, nri, is_emerg, rr_state)` in a new `msc_select.c`, unit-testable. Same NRI semantics (overlap allowed; with several matching usable links, least loaded, tie → lowest msc nr). No NRI → least loaded in round-robin order. None → existing reject path (counted). Pool mode off and no limits: identical results to today (proved by a test). |

**Tests**
* Lookup (D2): `sccp_backend`-style test (wrapped libosmo-sigtran) or a new unit test injecting N-UNITDATA.ind
  (RESET-ACK, PAGING) and N-CONNECT.ind on cs7 1 with the address of an MSC that also exists on cs7 0. Expected:
  attributed to the cs7-1 msc. Same MSC on different instances, plus duplicate on the same instance still refused.
* `msc_select` unit test: table of candidates × NRI × loads × eligibility × emergency, including the
  "identical to today" cases.
* VTY test: `max-connections` written/read back (`msc.vty`).
* E2E: **two links to one osmo-msc**, both CONNECTED; kill one worker → LU and SMS succeed on the other link with 0
  failures after detection; `max-connections 1` on one link → second concurrent transaction goes to the other link;
  all links full → reject counted, no crash.

**Acceptance (2c)**
1. With the fix, two cs7 links to the same MSC both reach BSSMAP CONNECTED and each message is attributed to the
   link it arrived on (E2E + unit). Existing configurations behave as before (all existing tests).
2. After one link fails, new LUs/SMS succeed on remaining OPERATIONAL links **without waiting for the replacement**.
   Count failures after the detection point: 0.
3. Per-link counters equal the number of live connections at all times (checked against the registry in tests).
   Over-limit links receive no new work. With all links full, requests are rejected and counted.
4. Pool mode off and no limits: `msc_select` gives the same choice as the old code (table test).

**Commits (≈4):** lookup fix (upstream-ready) · counters · limits · selection refactor + least-loaded.

## 6. Phase 2d: operations (drain, status, metrics)

| Task | Change |
|---|---|
| 2d-1 Drain | VTY (enable node) `a-worker-pool drain worker <0-15> [timeout <1-3600>]` and `a-worker-pool undrain worker <0-15>`, plus CTRL. DRAINING = ineligible; when the link counters reach 0 → GOODBYE → exit 0 → collected → **not restarted** (state STOPPED-drained) until `undrain`. Timeout → remaining connections released (counted, logged), then the same. |
| 2d-2 CTRL variables | `aworker-pool.state` (available/degraded/unavailable), per worker `aworker.N.{pid,state,cs7,health,restarts,fail-reason,failed-at,last-echo-ok}`, per msc `msc.N.{eligible,connections,max-connections}`. Read-only except drain/undrain/reset commands. |
| 2d-3 Counters | rate counters per worker: starts, crashes, liveness-kills, failures; pool: rejected (no eligible link). |

**Tests:** CTRL test (`ctrl_test_runner.py` style) for values and commands. Process test: drain with 0 connections →
exit 0, not restarted; drain timeout. E2E: drain the link carrying an idle subscriber → new work moves, drained worker
exits 0.

**Acceptance (2d):** drained links get no new work immediately; the worker ends with exit 0 and isn't restarted
until undrain; every documented CTRL variable reflects state within one poll; DevOps can observe everything without
signalling workers.

**Commits (≈3):** drain · CTRL · counters.

## 7. Phase 2e: end-to-end suite and upstream preparation

* **E2E suite:** extend the Phase 1 harness (real osmo-stp, osmo-msc ×1–2, hlr, mgw, bts-virtual, virtphy, mobile;
  isolated addresses) into a scenario runner covering the full test plan (architecture §16). Location: **[decision]**
  `tests/aworker_e2e/` in the osmo-bsc tree as an opt-in script (not part of `make check`), or the gsm-lab
  repository.
* **Upstream contribution (D2):** after 2c is green, prepare 2c-1 on top of current upstream osmo-bsc master with
  its unit test and a commit message in Osmocom style (Change-Id via the Gerrit hook). Submission to
  gerrit.osmocom.org is an outward action that needs your Gerrit account, so I prepare the patch and you submit it.
* **Interoperability checklist (D6):** a written procedure for the target MSC/STP vendors: several BSC PCs per MSC,
  RESET per PC, paging duplicates, paging response on another link, DUNA/DAVA behaviour. Production compatibility is
  claimed only per vendor after it passes.

**Acceptance (2e):** every "expected pass" scenario passes three runs in a row; "expected fail" scenarios behave as
documented (e.g. established call on the failed link is released, other links' calls untouched); measured detection
and recovery times recorded.

## 8. Proposed defaults (to confirm)

| Setting | Default | Rationale |
|---|---|---|
| `echo-interval` | 2 s | detection ≤ 6 s with 3 misses; negligible IPC load |
| `echo-misses` | 3 | avoids killing on one scheduling hiccup |
| `restart-delay` (base) | existing value (default 5 s) | compatible |
| `restart-delay-max` | 60 s | restart-storm protection |
| `stable-time` | 60 s | resets backoff |
| `max-restarts` | 5 in 300 s | then FAILED (worker-local) |
| `startup-timeout` | existing (20 s), per start | compatible |
| `protocol-timeout` (PROTOCOL_CONNECTING → restart) | 60 s | AS that never activates: assume worker-local only after this |
| `max-connections` per msc | 0 = unlimited | compatible |
| `drain-timeout` | 300 s | see §11.2: unrelated to the restart window |

## 9. Risks

* **DEGRADED vs. restart:** distinguishing worker-local from network causes is heuristic (the protocol timeout).
  Mitigation: never restart for N-PCSTATE/DUNA-driven loss; restart only on liveness, exit or protocol timeout.
* **SSNM on activation:** see the note in §4; reachability is proven by RESET-ACK.
* **Selection refactor** touches core MSC-pool code. Mitigation: pure function plus an equivalence table test against
  the old behaviour.
* **IPC version bump:** safe by construction (same binary); still covered by the existing exact-version test.
* **E2E timing flakiness:** use event-driven waits (as in Phase 1), and run each scenario three times.

## 10. Decisions needed before coding

1. Defaults in §8, including `protocol-timeout` 60 s.
2. E2E suite location: `tests/aworker_e2e/` (opt-in, in osmo-bsc) or gsm-lab.
3. After drain completes: keep the worker stopped until `undrain` (proposed), or restart it automatically?
4. Confirm the global-error list in §2, in particular "msc instance without owner" and "duplicate claim across
   files" as start-refusing.
5. Commit authorship for Phase 2 commits: the configured identity (`amrouhi24-hue`) as for `e54578f2c`, or
   `rouhi-dev` like the earlier branch commits.

## 11. Approval record and clarifications

Approved with these decisions:

1. **Defaults** as in §8: `echo-interval` 2 s, `echo-misses` 3, `restart-delay-max` 60 s, `stable-time` 60 s,
   `max-restarts 5 window 300`, `protocol-timeout` 60 s.
   * The **first** missed echo makes the worker ineligible for new connections at once; it is terminated when the
     number of consecutive misses reaches `echo-misses`.
2. **The two 300 s values are different things and never interact:**
   * `max-restarts 5 window 300` (restart storm protection): a sliding window over the times of *unplanned* worker
     restarts. A 5th restart that would fall within 300 s of the 1st puts the worker into FAILED. Planned stops
     (drain, BSC shutdown) are never counted.
   * `drain-timeout 300` (planned maintenance): the longest a DRAINING worker waits for its live connections to reach
     zero. When it expires, the remaining connections are released (counted, logged) and the worker is stopped. A
     drained worker is not restarted, so draining never adds to the restart window.
3. **E2E suite location:** gsm-lab. Unit, process-level and source-level regression tests stay in osmo-bsc.
4. **Drain:** a drained worker stops receiving new connections at once, lets existing ones finish, stops when its
   live count reaches zero (or at `drain-timeout`), is **not** restarted while drained, and rejoins only after an
   explicit `undrain` followed by full protocol readiness (§4).
5. **Global errors** (refuse BSC startup): two worker files claiming the same cs7 instance; an `msc` referencing a
   cs7 instance no configured worker owns.
6. **Commit author:** the configured identity (`amrouhi24-hue`).
7. **Process:** one independently reviewable change per commit; no new warnings; full suite green in normal and
   ASan/UBSan builds; Phase 1 tests unchanged; every new test shown to fail with its fix reverted; no push or
   upstream submission without explicit approval; stop if an acceptance criterion fails.

## 12. Progress log

### 2a-1 — GOODBYE delivery at shutdown: committed 3128774b5

Deviation from §2a-1 as planned: the plan queued GOODBYE from osmo-bsc's main loop once a shutdown is requested
and relied on libosmocore's pending-write accounting to flush it. The end-to-end test showed that this is racy:
`poll_disp_fds()` (select.c:407-424) declares the shutdown done at the end of the iteration in which the request
arrives if no write is pending at that moment, so a SIGTERM that lands while the loop dispatches ends the loop
before the hook runs. The flush therefore lives in `aworker_pool_stop()` (bounded non-blocking polling of the event
loop until `last_written_seq` covers each GOODBYE, then the grace period, then the Phase 1 SIGTERM/SIGKILL path);
`osmo_bsc_main.c` is unchanged. The E2E harness restart-delay was raised from 10 s to 20 s: the "SMS during
outage" round trip through the virtual radio stack takes 6–12 s, which exceeded the 10 s window once (run 5).

### 2a-1 follow-up — restart suppression while stopping: committed 04cd7d57a

Review requirement: prove deterministically that no replacement worker is spawned while `aworker_pool_stop()` polls
the event loop. Test P7 (tests/aworker_proc): a restart overdue at stop time, a pending GOODBYE so the flush polls,
a control timer proving that expired timers fire during the stop, and `fork()` counted via `-Wl,--wrap=fork`.
Two independent guards exist (`stopping` checked in `restart_timer_cb()`; restart timers deleted before the
flush); removing either alone is covered by the other, removing both or moving them after the flush fails P7.

### Note — E2E harness

The real-stack E2E harness used for Phase 1 and 2a-1 (`run_e2e.py`) lived in a session scratch directory that was
cleared. It was reconstructed from the session record (original file plus every later edit, replayed in order) and
re-run against 04cd7d57a: 46/46 checks, identical to run 7. It is now kept outside the repositories in
`~/dev/osmo-bsc-a-worker-pool-e2e/run_e2e.py` (run: `python3 -I run_e2e.py <workdir>`), until it moves into gsm-lab
in 2e.

### 2a-2 — expected ownership map and global checks: committed f487f1a3f

Before any fork, osmo-bsc reads every worker config file the way the worker does: a worker owns exactly the cs7
instances its file configures with an `asp`, `as` or `listen` node (`aworker.c` `cs7_collect()`). From those files it
builds the map from cs7 instance to expected worker. Two global errors from §2 now refuse to start, and the log
names the cs7 instance and the files and lines involved:
* a cs7 instance claimed by two worker files (the log names the instance, both workers, both files and both lines);
* an `msc` whose cs7 instance no worker file configures (an msc without an instance counts as instance 0, as in
  `osmo_bsc_sigtran_init()`).

A worker file that cannot be read is that worker's own error, not a global one. An msc instance that might be in
such a file is logged, not refused. `aworker_pool_check_ownership()` runs in `main()` right after
`aworker_pool_check_main_cs7()`. The start functions rebuild the map, refuse a duplicate and keep the map in the
pool. `aworker_pool_expected_owner()` exposes it. The check is deliberately not yet enforced against CS7_INFO
(that is 2a-3b).

Tests: `aworker_pool` T10 (real temporary config files; log text checked through a file log target). Five
mutations each fail T10. Removing the `main()` call is caught only by a real-binary run, not by `make check`: in that
run osmo-bsc started with an msc no worker serves. Real osmo-bsc: both bad configurations exit 1 before any fork.
Normal and ASan/UBSan 14/14, E2E 46/46.

### Test fix — P7 control timer: committed 489f1d234

P7 (04cd7d57a) scheduled a `struct osmo_timer_list` on the stack without zeroing it. `osmo_timer_setup()` sets
only `cb` and `data`, so `osmo_timer_add()` could find stack garbage in `active` and call `rb_erase()` on a node that
was never in the timer tree. The 2a-3a library change altered the stack layout, after which the P7 child crashed
(SIGSEGV in `rb_erase()`) in every normal-build run. The ASan build happened to hide the crash. The fix
zero-initialises the timer. It is a test-only change committed on its own before 2a-3a, and passes 14/14 in both
builds. With the pending 2a-3a change applied, P7 passed 5 of 5 runs, where it had crashed in every run before.

### 2a-3a — worker-local FAILED state and operator reset: committed bb797c02e

Documented behaviour change: with `on-worker-failure restart` (the default), a worker that exits with
`AWORKER_EXIT_CONFIG` (66) no longer shuts osmo-bsc down. It enters `AWORKER_ST_FAILED`, and osmo-bsc and the other
workers continue. `on-worker-failure shutdown` remains the explicit opt-in that ends osmo-bsc on any worker failure.
* FAILED is entered only after the process is collected (Phase 1 rule), in `worker_apply_policy()`. The cause is
  either the exit status or a reason recorded in `pending_fail` before the link was closed.
* It records the reason (`config`, `ownership`), a detail text and the wall-clock time.
* FAILED is terminal: no restart timer is armed, and the startup timeout skips FAILED workers.
* Reset: `aworker_pool_reset_worker()` and VTY (enable node) `a-worker-pool reset worker <0-15>` forget the failure and
  start only that worker. A reset of a worker in any other state is refused (`-EBUSY`, "% worker N is not FAILED").
* Visibility: `show a-worker-pool` prints `failed: <reason>, at <time>: <detail>` and how to reset.
  `aworker_pool_worker_info()` carries `fail_reason`, `fail_detail` and `failed_at`.

Deviations from §2:
* An exec failure in the child exits 66 and is therefore recorded as `config`, not as a separate `exec` reason.
* The `point-code` reason (worker point code differs from the main file) is not implemented yet. The mismatch is
  still only logged.

Tests: `aworker_proc` P8 (real fork/exec). Worker 1 exits 66 and becomes FAILED with reason, detail and time, while
worker 0 stays READY. No `fork()` happens past restart-delay and startup-timeout. A reset of a READY worker is
refused. After the file is fixed, a reset forks exactly once and the worker becomes READY. Worker 0 keeps its
process, and no shutdown is requested. Seven mutations each fail P8. Real osmo-bsc: 13/13 scripted checks (show
output, refused resets, a reset while still broken fails again after one start, a reset after the fix gives READY,
worker 0's pid unchanged, exit 0). Normal and ASan/UBSan 14/14.

The process tests are numbered in commit order (P5 GOODBYE, P6 lingering workers, P7 restart while stopping, P8
FAILED/reset, P9 ownership), not as listed in §3.

### 2a-3b — runtime CS7_INFO ownership validation: committed 305f72cc2

`handle_cs7_info()` checks each claim against the 2a-2 map. On a mismatch it closes the link with `pending_fail =
ownership`, and once the process is collected the worker is FAILED, only that worker, until reset.
* A claim of an instance that another worker's file configured is refused, even before that worker has reported.
* A worker whose expectation is known (its file was readable and configures at least one instance) must report
  exactly that set. Extra and missing instances are both refused, and the detail says which.
* A worker with no known expectation keeps the Phase 1 first-come duplicate check. That covers no config file (as
  with `aworker_pool_start_with_fds()`), an unreadable file, or a file that configures nothing. A mutation that
  treats a file configuring nothing as an empty known expectation fails Phase 1 test P1, which is why the rule
  exists.

Tests:
* `aworker_pool` T11 (scripted workers, real config files): an extra claim, a missing instance and a foreign claim
  each make worker 1 FAILED with that detail, while worker 0 stays READY and its SCCP user available. A matching
  claim is accepted.
* `aworker_proc` P9: a mismatch leads to FAILED with no `fork()`. With a matching report restored, a reset makes the
  worker READY. Worker 0 is untouched.

Mutation results:
* O1–O3 fail both T11 and P9.
* O4–O5 fail T11 only; P9 does not cover those cases.
* O6 fails Phase 1 test P1.

Real osmo-bsc, 12/12 scripted checks: worker 1's file was changed at runtime and the worker SIGKILLed. Its
replacement went FAILED (ownership), worker 0's pid stayed the same, and restoring the file plus a reset gave READY.
T1–T10 and P1–P8 unchanged. Normal and ASan/UBSan 14/14, no new warnings, E2E 46/46.

### Restart policy semantics, approved before 2a-4 (2026-11-15)

* **Restart limit:** §11.2 is applied literally (decision recorded before coding, because the 2a-4 brief's
  "maximum 5 restarts" could be read the conventional way). The time of each unplanned restart is recorded per
  worker (CLOCK_MONOTONIC, when the lost process is collected and the restart is decided). A recorded restart
  counts while it is less than `window` seconds old. An unplanned loss that would be restart N within the window
  of the oldest restart still recorded makes the worker FAILED (`restart-limit`). So at most N-1 restarts happen,
  4 in 300 s with the defaults. Config and ownership failures are FAILED directly. Planned stops (and later
  drains) never count.
* **Stable time** resets only the backoff exponent, as §3 says. The restart record expires by age only. An
  operator reset clears both.
* **`protocol-timeout` (60 s)** belongs to 2b (PROTOCOL_CONNECTING) and is not part of 2a-4. The startup timeout
  stays separate from both the stable time and the restart window.

### 2a-4a — exponential restart backoff: committed eef2bac51

`delay = restart-delay · 2^n`, capped at `restart-delay-max` but never less than `restart-delay`.
* n counts the worker's consecutive unplanned restarts.
* n resets when the worker was READY for at least `stable-time` before it was lost, measured from READY to the
  loss on CLOCK_MONOTONIC. Time spent STARTING never counts, so a startup timeout continues the backoff.
* A start that fails before any process exists (socketpair()/fork()) retries after the current delay, at least
  1 s, without advancing n.
* New commands: `restart-delay-max` (60) and `stable-time` (60). `restart-delay` (5) keeps its meaning as the
  first delay.
* Status: the start log, `show a-worker-pool` and `aworker_pool_worker_info()` (`backoff_step`, `restart_delay`).

Tests: `aworker_proc` P10–P12. In the osmo-bsc process of these tests the libosmocore clocks are overridden
(`osmo_gettimeofday_override`, `osmo_clock_override_*`), so timers and CLOCK_MONOTONIC only move when the test moves
them. Every delay is checked to the millisecond: no start 1 ms before it, exactly one at it.
* P10, defaults: 5, 10, 20, 40, 60, 60 s.
* P11, stable time: READY 59.999 s → 10 s; READY 60 s → back to 5 s.
* P12: a never-READY process that runs 65 s until the startup timeout continues the backoff (5, 10, 20 s).

Nine mutations each fail P10, P11 and/or P12. Real osmo-bsc: 12/12 scripted checks (2, 4, 8 s, shown in
`show a-worker-pool`; worker 0's pid unchanged). Normal and ASan/UBSan 14/14, E2E 46/46.

### 2a-4b — restart limit: committed ef8b53898

`max-restarts <1-32> window <1-86400>` (5, 300) with the semantics above, leading to FAILED (`restart-limit`).
* The detail names the restarts, the age of the oldest, the limit and the last loss reason.
* Status: the limit and its meaning in the header ("a loss that would be restart 5 within 300 s makes a worker
  FAILED (at most 4 restarts)"), restarts in the window per worker, and `restarts_in_window` in the info.
* The backoff line is no longer printed for a FAILED worker.

Tests:
* P13, defaults: restarts after 5, 10, 20, 40 s, then FAILED at the loss that would be restart 5. Nothing starts
  in the 200 s after, while the startup timer fires. A reset with all 4 restarts still in the window starts it
  once, and the next loss waits 5 s.
* P14, `max-restarts 2 window 300`, two workers restarted at 0 s: a loss at 299.999 s makes one FAILED, a loss at
  300 s restarts the other, and the status stops counting the old restart at 300 s.
* P15, `max-restarts 1`: a loss while running is FAILED, while the same loss handled inside `aworker_pool_stop()`
  counts nothing.

Mutations: ten, each failing P13, P14 and/or P15, one of them also P10. L10 (status counts expired restarts)
survived the first run, so P14 gained the 300 s status check, which now catches it. Real osmo-bsc,
`max-restarts 3 window 120`: 13/13 scripted checks (FAILED on the 3rd loss, VTY reset to READY, worker 0's pid
unchanged). Normal and ASan/UBSan 14/14.

E2E: the first run on ef8b53898 failed one check, "[kill-2] BSC descriptor count unchanged (16, now 15)". The
extra descriptor was in the *baseline*, not afterwards. The baseline is taken right after `pool()` closes a VTY
connection, and osmo-bsc releases its end only when its event loop handles the EOF. A standalone measurement saw
that end still open right after close() in 184 of 200 tries. This is a race in the harness, not in the code, and
it was present since run 1. The harness now waits for the baseline to settle, as it already did for the count
afterwards, so the check is unchanged. After the fix: two runs, 46/46 each.

### Documentation and tooling

* The user manual chapter (`doc/manuals/chapters/aworker-pool.adoc`) was behind the code. It still said that a
  worker that cannot use its configuration shuts OsmoBSC down, which has been wrong since 2a-3a. It now
  describes the start-time ownership checks (2a-2), FAILED with its reasons and the reset (2a-3), and the restart
  backoff and limit (2a-4), and its `show a-worker-pool` example matches the current output. The manual build
  was not run (no asciidoc toolchain in this environment).
* The mutation driver now lives next to the E2E harness: `~/dev/osmo-bsc-a-worker-pool-e2e/mutate.py`.

### Open points after 2a-4

* Drain (2d) does not exist yet. That drains never count is a design rule, not yet tested.
* The startup timeout is still one pool-level timer, armed at pool start (per start: 2a-5).
* The retry after a start failure without a process (fork() failing) has no test.
* `restart-delay 0` stays 0 (compatible). Such a worker can therefore restart N-1 times without delay before the
  limit makes it FAILED.
* The worker-level `point-code` failure reason (§2) is still not implemented.

### Policy B confirmed; P16: committed 2a8e2e0a4

Reviewed after 2a-4 and approved: stable time resets only the backoff exponent. Restart records leave the window only
by age, or through an operator reset (§3, §8, §11.2; architecture §6). Test-only commit P16:
* Lost at 0, 5 and 15 s, then READY 60 s and lost at 95 s: restarted after 5 s. The earlier restarts are still in
  the window and the new one is added, 4 in the window.
* READY 60 s again and lost at 160 s: FAILED (restart-limit).
* Clearing the record after a stable period (Policy A) fails P16 and P14. Not resetting the backoff fails P16,
  P11 and P14.

### 2a-5 — startup timeout per start: committed 6b36f3301

Each worker has a startup timer for its current process.
* **Armed** in `link_create()`, which every start (pool start, restart, reset, test start on existing sockets)
  goes through. Every start therefore gets the full `startup-timeout`, and re-arming replaces an earlier deadline.
* **Deleted** on every way out of STARTING: READY, `worker_lost()`, and `aworker_pool_stop()` (before the
  GOODBYE flush).
* **Callback:** acts only on its own worker, only while it is STARTING, and ends it with
  `worker_lost(w, "startup timeout")`. Collection and the failure policy (backoff, restart limit, FAILED,
  `on-worker-failure shutdown`) then apply as for any other unplanned loss.
* **Pool-level timer:** kept only for its diagnostic about SCCP users still unbound after pool start.
* **Default:** stays 20 s as approved in §8. The 2a-5 brief said 60 s; changing the default was not done without
  approval.
* **Behaviour change:** restarted and reset workers are now timed out as well.

Tests (fake clock, `startup-timeout 60`):
* **P17:** two workers started at 5 and 25 s, neither READY. At 64.999 s both are still starting. At 65 s only
  worker 0 is timed out, as an unplanned loss with backoff 10 s, then restarted and READY. Worker 1 is timed out at
  85 s. Each worker's pid and state are untouched by the other.
* **P18:** READY at 64.999 s is not timed out (checked up to 300 s).
* **P19:**
  * A process lost before its deadline: neither its old deadline nor a deadline passing during RESTARTING
    affects the worker or its replacement.
  * The replacement is timed out exactly at its own deadline.
  * A stop with a process STARTING leaves no live timer; the fake clock keeps running through the stop and past the
    deadline.
* **P12:** now uses `startup-timeout 65` per start, with the same timeline and output.

Mutations:
* **Caught:**
  * armed only at the first start;
  * no cancel at READY and no state check;
  * no cancel on loss and no state check;
  * no cancel on loss and the deadline not replaced at the next start;
  * a timeout ending every STARTING worker;
  * stop leaving the timers pending (ASan build only; the normal build reads freed memory without crashing).
* **Survive by design (as P7):** removing one of the two guards alone, i.e. the cancellation at READY, the
  cancellation on loss, or the state check.

Two test corrections while writing them, neither of which weakened a check:
* P18 first expected 1 restart in the window at 300 s. At 300 s the restart from 0 s has correctly left the
  window, so "no further loss" is now checked at 250 s and "same process" again at 300 s.
* The stop check first passed even with the stop-path deletion removed. Its worker exited on GOODBYE, so
  `worker_lost()` already cancelled the timer. Also, libosmocore is not ASan-instrumented and the deadline lay far
  in the future. The process now lingers past the flush, and the clock passes its deadline.

Normal and ASan/UBSan: 14/14, no new warnings, no sanitizer report. E2E: 46/46.

Open points:
* No real-binary test of the startup timeout: a real worker reports its cs7 instances within milliseconds, so it
  cannot be held in STARTING without a test seam.
* `show a-worker-pool` does not show the remaining startup time (2a-8, status).

### Startup-timeout default 60 s (decision after 2a-5)

The approved 2a-5 instruction (an independent 60 s timeout for every worker start) supersedes the 20 s in §8. The
default is now 60 s; an explicit `startup-timeout` still replaces it. The manual and the example configuration say so.
* Test P20 (fake clock):
  * The built-in default is 60. A process started at 5 s is still starting at 64.999 s and is timed out at 65 s.
  * With `startup-timeout 30` it is timed out at 35 s.
* Mutations:
  * Default back to 20: fails P20.
  * The configured value ignored: fails P20.
* Real osmo-bsc:
  * Without the command, `show a-worker-pool` says 60 s and `show running-config` writes no line.
  * With `startup-timeout 30`, both show 30.

### S1, S2-0, S2-1 and S2-2

* **S1** (source separation into `src/a-worker/libaworker.la`, no behaviour change): closed and accepted. As built:
  SUP §14 ("As implemented").
* **S2-0 review:** the subphase structure and decisions of §14.1–§14.2.
* **S2-1** (view builder `aworker_cfg_view.c`, inline form `worker <0-15> cs7-instance <0-15>`, D-S1 rules, BSC
  view parsing) and its **Option C follow-up** (every configuration write refused in inline mode, VTY through
  `config_is_consistent` and CTRL through `verify_net_write_config_file()`; new test `tests/aworker_cfg_write`): closed
  and accepted. The planned worker-owned `route-table` filter was dropped: `route-table` is no osmo-bsc command (SG
  role only, SUP §4.1). Limitations carried into S2-2 and resolved there: the C8 hook needed a real-binary test (R-2),
  `show startup-config` in inline mode (D-8), and the missing official transcript runner (D-10).
* **S2-2** (single read, worker snapshots, legacy mode): steps 1–8 implemented and verified, step 9 documentation
  (`b48979738`), step 10 final verification passed (`9412b2f15`); accepted. Record and results in §14.5.
* **S2-3** (standalone `bsc-aworker`): stages 1–5 implemented, verified and committed one by one; record in §14.6.
* **S2-4** (IPC 2.0 and descriptor passing): stages 1–4 implemented, verified and committed one by one; record in
  §14.7.
* **Commits:** `15c749359` "aworker: implement shared config and worker snapshots" (S1, S2-1 and S2-2 steps 1–6),
  `5f4483f0c` "aworker: integrate snapshots with pool workers" (step 7), `b4d890241` "aworker: record the config path
  in inline mode (D-8)" (step 8). The first one combines several subphases, against the "one change per commit" rule of
  §13.1, by the project owner's decision: they share files and dependencies, and a split commit would not build.
  Pushed to `origin` on 2026-11-17 (`e54578f2c..b4d890241`, together with the 2a-1 … 2a-5 commits). The later commits
  (S2-2 steps 9–10, S2-3, S2-4) are local, not pushed.

## 13. Revised plan: standalone Supervisor (S1 closed; S2 in progress, see §12 and §14)

Architecture, decisions and risks: `PHASE2_SUPERVISOR.md` revision 3, referred to below as SUP. Starting point: HEAD
`fa01448ea`.

### 13.1 Rules for every phase

* The §1 ground rules apply unchanged:
  * one reviewable change per commit;
  * no new warnings;
  * normal and ASan/UBSan `make check` green;
  * every new test shown to fail under a mutation of its fix;
  * documentation in separate commits;
  * no push, no history rewrite.
* **P1–P20 and T1–T11 keep their sources, meaning and `.ok` output**, unless a commit deliberately changes a
  documented behaviour and says so. P2/P3 are never inverted.
* **Legacy in-process mode stays the default.** After every phase it passes its suites and the external E2E harness
  (46 checks).
* No systemd, service, cgroup or deployment work (SUP §15).
* A phase starts only after explicit authorization, and after the decisions it depends on (§13.6) are approved.

### 13.2 Phases and dependencies

| Phase | Content | Depends on |
|---|---|---|
| **S0** | Architecture and decisions: SUP rev. 3, this §13, decision table SUP §16. Documentation only. | — |
| **S1** | **Mechanical source separation, no behaviour change** (SUP §14): `src/a-worker/libaworker.la` (IPC, link, scanner, VTY definitions, worker role, lifecycle part of `aworker_pool.c`, shared log category table); user glue stays in `libbsc.la`, connected through the callback seam; decoupling from `tall_bsc_ctx`, `DAWORKER` and `gsm_data.h`; build integration (`src/Makefile.am`, `configure.ac`). No `bsc-aworker` yet; no new behaviour. | S0 approved |
| **S2** | **Shared configuration:** view builder (SUP §4.2); inline form `worker I cs7-instance X` and conflict rules (D-S1); BSC view parsing in inline pools; snapshot builder (memfd, D-S2); legacy mode can use the inline form (N2). *Amended by §14:* the canonical document and its comparison move to S4 (subphase S2-5), and the `on-worker-failure shutdown` refusal moves to S3 (S2-3). | S1 |
| **S3** | **Standalone `bsc-aworker`:** `aworker_main.c`; Supervisor view and logging (D-S10); lifecycle from `libaworker.la`; worker role by self-re-exec (same-binary guarantee) parsing its snapshot; pool start, restart, FAILED, reset, startup timeout and pool shutdown, as today but without any BSC; no runtime management VTY server (D-S6). *Amended by §14:* the operator control channel moves to S4 (S2-5), together with the socket security policy (N5); in S3 the Supervisor is controlled by signals only. No sessions yet. | S2 |
| **S4** | **Session IPC and fd passing:** IPC v2 (major/minor, nonce, incarnation, session_id, message classes); SCM_RIGHTS per SUP §8.3; session state machine and session ECHO (SUP §9); ATTACH/DETACH (worker side minimal: close the data link and unbind, without the release sequence); WORKER_UP/DOWN/STATE; operator FENCE_SESSION with pidfd. | S3 |
| **S5** | **Detached worker:** protocol test peer (§13.4) first, then the release sequence, counters, confirmation window and reattach (SUP §10). The `detach-release-timeout` default is decided here (D-S9). | S4 |
| **S6** | **BSC integration** (opt-in `mode supervisor`): session client; canonical document in SESSION_OPEN; exit on reject; data links into the existing user, bind and TX code; WORKER_DOWN/UP handling; SESSION_CLOSE on planned stop (no GOODBYE); reconnect with backoff after Supervisor loss; reset and status forwarding. The BSC never spawns or signals workers in this mode. | S5 |
| **S7** | **Full E2E fault-tolerance acceptance** (§13.5), using the external real-stack harness, extended | S6 |

S1 → S2 → S3 → S4 → S5 → S6 → S7, strictly sequential, including the subphases S2-1 → … → S2-8 (§14). The S5 protocol
test peer is built inside S5 (S2-6), not in parallel with earlier phases.

### 13.3 Test gates

| Phase | Gate (all must hold) |
|---|---|
| S1 | P1–P20, T1–T11 and all other suites: output byte-identical to `fa01448ea`. Legacy E2E 46/46. `git diff -M` shows only moves, the callback seam and the parameterisation; reviewed per function against SUP §14.3. `osmo-bsc --help`, `show running-config` and the log output of an E2E run are unchanged (same categories, same prefixes). No change to `.ok` files. |
| S2 | Golden-file view tests (SUP §4.3), including every refusal case. The BSC view of an inline configuration parses to the same objects as the equivalent legacy main file. Each worker view parses to the same objects as the equivalent legacy worker file. Canonical document: identical across whitespace and comment changes; different on any value change; explicit defaults in P; BSC-only keys excluded. D-S1 conflict rules (both forms on one worker, mixed pool, file form in supervisor mode, transport command outside worker-owned instances) each refused with a message naming the line. Mutation per rule. |
| S3 | P1–P20 semantics re-run against the real `bsc-aworker` where the test drives a parent (spawn, isolation, PDEATHSIG on **Supervisor** death (P2/P3), same image (P4), restart policy, FAILED, reset, startup timeout, pool shutdown with GOODBYE and exit 0). Workers parse only their snapshot (shown by modifying `osmo-bsc.cfg` after Supervisor start: restarts unaffected; the CS7_INFO check still active, by mutation). No listening TCP socket (no runtime VTY server, D-S6). Logging per D-S10, including the shared-file line-atomicity test. *(The operator CLI test moves to S4/S2-5, §14.3.)* |
| S4 | fd passing unit tests: arrives intact; `MSG_CTRUNC`, missing, extra or unexpected descriptors close the link and leak nothing (descriptor counts as in Phase 1 P1); descriptor kept until send completion. Session tests: accept; reject for busy, version and config-mismatch; UNRESPONSIVE never admits a second BSC; CLOSING runs exactly once for every race in SUP §9.1 (mutation: duplicate teardown detected); fencing with confirmed exit admits; unconfirmed or no pidfd refuses; worker ECHO loss affects only that worker; session ECHO loss never touches workers. |
| S5 | §13.4 in full; DETACHED/ATTACH cycles without descriptor or memory growth (ASan, descriptor counts) |
| S6 | Attach → bind → RESET; BSC SIGKILL → Supervisor and worker PIDs and incarnations unchanged; reattach rebinds and RESET/RESET ACK completes; worker restart under a live session produces DOWN/UP and only that cs7 is affected; planned BSC stop sends no GOODBYE; BSC exits on reject; legacy mode unchanged |
| S7 | §13.5, three consecutive full runs |

### 13.4 Detached-mode protocol tests (S5) and the timeout decision (D-S9)

**Test peer:** a test-side libosmo-sigtran M3UA server (or STP-side ASP) plus an SCCP endpoint on SSN 254. It can:
* open connections towards the worker and accept connections from it;
* answer or withhold CC, RLC and RLSD;
* send CR, UDT and SST;
* record every received message with a timestamp.

Wire observation is at the peer, so it does not depend on the worker's own reporting.

For each case the test records separately:
1. API acceptance (return value);
2. wire transmission (seen at the peer);
3. peer response (sent by the peer);
4. local removal (`osmo_sccp_conn_id_exists()`);
5. the reported counter.

| Case | Setup | Expected / to be measured |
|---|---|---|
| A1 ACTIVE, peer answers RLC | N connections ACTIVE | RLSD per connection at the peer **before** any unbind-caused UDTS/CREF; local removal on RLC; `confirmed = N`; measure the RLSD → removal latency distribution |
| A2 ACTIVE, peer withholds RLC | same, peer silent | RLSD once; T(rel) retransmission observed only if the window exceeds T(rel); unbind at window end; no RLSD after the unbind; `unconfirmed = N` |
| A3 ACTIVE, peer answers late | RLC delayed past the window | counted `unconfirmed`; the late RLC hits an unknown reference (Table B.2: discard) [V `sccp_scoc_rx_unass_local_ref()`] |
| P1 CONN_PEND_OUT | the worker's CR outstanding (peer withholds CC) | no RLSD at detach (source finding); `pending_conn_conf`; then the peer sends CC after the unbind: record what the worker sends (expected ERR or nothing; to be observed) |
| P2 CONN_PEND_IN | the peer's CR delivered, no answer yet | CREF sent; `refused_incoming` |
| W1 WAIT_CONN_CONF reached before detach | as P1 with an earlier disconnect | as P1 |
| D1 DISCONN_PEND before detach | a release already in progress | no second RLSD from the detach; counted per its outcome |
| T1 Transport down | ASP inactive or SCTP down at detach | `not_issued_transport_down` (detection method validated); no crash; detach completes |
| T2 AS-PENDING | AS pending at detach (if reproducible) | observe whether RLSDs are queued, sent later or dropped (closes the untraced-leg risk) |
| R1 Retransmission timers | window longer than T(rel) | RLSD repeats at T(rel)/T(repeat_rel) as in `scoc_fsm_disconn_pend()`, and stop at the unbind |
| E1 Window expiry | each case with window 0 and with a longer window | unbind exactly at expiry; counters consistent |
| U1 After the unbind | — | UDT → UDTS "unequipped user"; CR → CREF "unequipped user"; SST → no answer; no SSP |
| U2 During the window | — | CR → CREF (refused by the worker); SST → SSA (the documented trade-off; measured duration) |
| RA Reattach | after each case | USER_BIND succeeds; traffic resumes; no state from the old session |

**Decision rule for D-S9:**
* The default is chosen from the A1 latency distribution, measured at the peer, under representative load and RTT,
  and from the U2 false-availability window.
* The values are recorded in this log with the measurements.
* Until approved, supervisor mode requires an explicit value.

Vendor MSCs: the UDTS/CREF sequence (U1) must be shown per vendor before compatibility is claimed (D6).

### 13.5 S7 acceptance: full scenario, three consecutive runs

| # | Step | Measurable acceptance |
|---|---|---|
| 1 | Start the Supervisor and workers | Supervisor PID recorded; every worker READY within startup-timeout; one process per worker, all children of the Supervisor |
| 2 | Start the BSC and attach | SESSION_ACCEPT logged with session_id; WORKER_UP for every worker; BSC `show a-worker-pool` lists all workers attached |
| 3 | Workers ready, ASP ACTIVE | STP `show cs7 instance … asp` shows every worker ASP ACTIVE; RESET/RESET ACK per MSC |
| 4 | Real signalling | location update and MO+MT SMS succeed (counters increase, as in the current harness) |
| 5 | SIGKILL the BSC | the BSC process is gone and collected |
| 6 | Supervisor unchanged | same PID; session CLOSING → CLOSED logged exactly once |
| 7 | Workers unchanged | same PIDs and incarnations; every worker DETACHED (counters logged) |
| 8 | Transport unchanged | no ASP down/up, AS failover or SCTP restart in the STP log for the whole interval; same SCTP associations (local/remote ports) seen on the worker side before and after |
| 9 | Detached MSC behaviour | after the first UDT towards the BSC, the osmo-msc log shows N-NOTICE "unequipped user" and the ran_peer goes to WAIT_RX_RESET; no transport change |
| 10 | Restart the BSC with the same file | SESSION_ACCEPT, new session_id |
| 11 | Same worker incarnations | WORKER_UP carries the incarnations of step 7; worker PIDs unchanged |
| 12 | RESET recovery | RESET/RESET ACK per MSC; osmo-msc ran_peer READY |
| 13 | MO and MT SMS | both succeed after recovery |
| 14 | Single worker failure | SIGKILL one worker: only that worker is restarted (new incarnation, backoff per policy), WORKER_DOWN/UP seen by the BSC; other workers keep their PIDs and ASPs; traffic over the others continues |
| 15 | SIGKILL the Supervisor | every worker exits (PDEATHSIG) within the grace bound; no orphan, no zombie; their ASPs go down at the STP; the BSC logs the session and worker loss and starts reconnecting |
| 16 | Restart the Supervisor | a fresh pool (new nonce, new incarnations); the BSC reattaches; RESET; SMS works |

Also in every run:
* a planned BSC restart (SIGTERM) behaves like steps 5–13 without GOODBYE to workers;
* a planned pool shutdown through the Supervisor ends every worker with exit 0 after GOODBYE (2a-1 semantics);
* the subscriber connections and SCCP connections active at the moment of the BSC crash are lost (documented
  exclusion).

### 13.6 Decisions per phase, and resuming Phase 2

* **Before S1:** S0 approved (SUP §14 layout and seam). Nothing else.
* *Superseded by §14.4*, which gives the decision deadlines per subphase. Summary: D-S1, D-S2 and N2 are approved
  (before S2-1/S2-2); D-S10 before S2-3; the canonical document scope (CD-1), D-S12 and N5 before S2-5; D-S9 before
  S2-6 sign-off.
* **Not needed for the MVP:** D-S7.
* **2a-6 onwards resumes only after S7 has passed.** Ownership then follows SUP §14.4 and §9.2:
  * 2a-6 ECHO in Supervisor ↔ worker;
  * 2a-7 in both;
  * 2a-8 status split;
  * 2b: readiness reported by workers, protocol-timeout restarts by the Supervisor, RESET-ACK gating by the BSC;
  * 2c in the BSC;
  * 2d decided by the BSC and executed by the Supervisor.

  Any earlier resumption needs explicit approval.

## 14. S2-0 review: implementation subphases S2-1 … S2-8 (approved structure; S2-1 done, S2-2 in progress)

### 14.1 Naming

§13 stays the authoritative phase numbering: S2 Shared configuration, S3 Standalone executable, S4 Session IPC and fd
passing, S5 Detached worker, S6 BSC integration, S7 E2E. **S2-1 … S2-8 are implementation subphase identifiers
only.** Their mapping:

| Subphase | Plan phase | Content (short) |
|---|---|---|
| S2-1 | S2 (part 1) | view builder, inline syntax, conflict rules, BSC view parsing |
| S2-2 | S2 (part 2) | snapshots (memfd + fallback); inline form in legacy mode |
| S2-3 | S3 | standalone `bsc-aworker`, worker role, header split, no sessions |
| S2-4 | S4 (part 1) | IPC v2, message classes, SCM_RIGHTS primitives |
| S2-5 | S4 (part 2) | control/data link split, sessions, operator channel, fencing, canonical document |
| S2-6 | S5 | detached-worker release (protocol test peer first) |
| S2-7 | S6 | BSC session client (opt-in supervisor mode) |
| S2-8 | S7 | full E2E acceptance |

**Order: strictly sequential, S2-1 → S2-2 → S2-3 → S2-4 → S2-5 → S2-6 → S2-7 → S2-8.** No subphase starts before
the previous one is accepted and before the next one is explicitly authorized. S2-4 does not run in parallel with
S2-1.

### 14.2 Decisions recorded at the S2-0 review

| ID | Decision | Status |
|---|---|---|
| D-S1 | Inline and legacy configuration rules: **one configuration form per worker; mixing forms within a pool is an error; supervisor mode accepts the inline form only; legacy mode supports both forms** | APPROVED |
| N2 | The inline form is supported in **legacy mode** as a stepping stone (S2-2), before process ownership moves | APPROVED |
| D-S2 | Worker configuration snapshots as **sealed memfd**, with a **tested unlinked-file fallback** | APPROVED |
| N3 | The **existing legacy worker argv contract** (`--aworker-fd N --aworker-index N --aworker-config FILE`, parsed by `osmo_bsc_main.c` and by the P-test fake worker) stays **unchanged**. The standalone executable may use its own role/argv contract | APPROVED |
| N4 | **Split** the lifecycle public header from the BSC-specific (SCCP user) public header **during S2-3**. No mechanical move of other S1 functionality (`aworker_pool_check_main_cs7()`, `show`/`reset` VTY commands stay where S1 put them) | APPROVED |
| D-S5 | Prefer **deterministic canonical-document comparison** over hash-only comparison, **subject to defining its exact contents** (CD-1, §14.4) | APPROVED (contents open) |
| D-S6 | **No runtime management VTY server** in the Supervisor. **Configuration VTY command registration remains** where it is needed to parse the configuration | APPROVED |
| D-S8 | `on-worker-failure shutdown` is a global configuration error in supervisor mode; legacy unchanged | APPROVED (unchanged) |
| D-S11 | The standalone Supervisor architecture supersedes the earlier statements | APPROVED (unchanged) |

Decisions after the S2-2 implementation start, at the S2-3 design review (2026-11-17):

| ID | Decision | Status |
|---|---|---|
| B1 | Reopen S2-2 and complete its remaining approved steps (snapshot delivery) before S2-3; S2-3 must not absorb S2-2 work | APPROVED; done through step 8 |
| B2 | D-S10 logging: workers per D-2 (no log file, stderr inherited from the parent); Supervisor keeps its targets incl. files; BSC unchanged; shared file tested for default and `blocking-io` (ext4, kernel 7.0.0-43); **`wq` unverified**; durable regression test required; legacy log output unchanged | APPROVED with limitations (SUP §7) |
| B3 | Executable name **`bsc-aworker`** everywhere (Automake, sources, docs, tests, examples, re-exec); never `osmo-bsc-aworker`. Runtime names `bsc-aworker-<i>` (memfd), `bsc-aworker-snap.<random>` (fallback) | APPROVED (final) |
| B4 | Worker-role arguments: keep `--aworker-fd` and `--aworker-snapshot-fd`; internal, strictly validated, conflicting combinations refused; legacy `--aworker-config FILE` unchanged; FD ownership and close-on-exec rules | APPROVED |
| B5 | Identity prefixes (`aworker-supervisor`, `aworker <idx> inc <n>`) for the new executable only; legacy output unchanged | APPROVED |

The S2-2 decisions (D-1 … D-13, SR, FD, U-1 … U-6 and the final decisions of 2026-11-17) are in §14.5.

**Not approved, still open:** D-S9, D-S12, N5, and the canonical document contents (CD-1). D-S3 and D-S4 remain as
before (§14.4). D-S10 is resolved by B2.

### 14.3 Subphases

Common to all subphases: the §13.1 rules (one change per commit, no new warnings, normal and ASan/UBSan suites, mutation
testing for new behaviour, P1–P20/T1–T11 meaning and output unchanged, legacy mode the default, legacy E2E 46/46
after every subphase). Each subphase is additive or opt-in until S2-7, so legacy mode keeps working without it. Until
S2-2 step 8 nothing was pushed; since 2026-11-17 the branch is on `origin` (§12), so a rollback is a revert commit,
not "do not merge".

#### S2-1: view builder, inline syntax, BSC view

* **Objective:** `osmo-bsc.cfg` can declare `worker <0-15> cs7-instance <0-15>` and contain the worker-owned cs7
  instances including their `asp`/`as`/`listen` nodes. A shared view builder produces the BSC, Supervisor and worker
  views (SUP §4.2), and the BSC parses its view. No process ownership change; no spawning change.
* **Files and functions:**
  * `src/a-worker/aworker_cfg_scan.c` and a new `src/a-worker/aworker_cfg_view.c` (+ `include/osmocom/bsc/aworker_cfg_view.h`):
    view building with the parser's rules (`fgets()` into `VTY_BUFSIZ`, `cmd_make_strvec2()`, prefix indentation
    as in `indent_cmp()`, removed lines replaced by empty lines);
  * `src/a-worker/aworker_vty.c`: the new command and its writer;
  * `src/a-worker/aworker_pool.c`: `ownership_build()`, `aworker_pool_prescan_config()`,
    `aworker_pool_check_ownership()` for the inline form and the D-S1 rules;
  * `src/osmo-bsc/osmo_bsc_main.c` `bsc_read_config()`: parse the BSC view with `vty_read_config_filep()`.
* **Parser separation (clarification B):** the view builder only selects lines; the **real** libosmovty and
  libosmo-sigtran parsers interpret them.
* **Tests to add:**
  * `tests/aworker_cfg_view/` golden-file tests for the views, covering every case of SUP §4.3;
  * **parse validation against the real parser** (clarification B):
    * every generated BSC view is parsed with `vty_read_config_filep()` and the real node set
      (`osmo_ss7_vty_init_asp()`, `osmo_sccp_vty_init()`, `aworker_pool_vty_init()`). The resulting objects are
      checked: the cs7 instances exist with the expected point code and SCCP address book, and **no ASP or AS
      object exists** in the parsing process;
    * every generated worker view is parsed the same way, in a separate child process per view, because parsing an
      `asp` node starts it; the test configurations point the ASPs at an unused loopback port. The resulting ASP,
      AS and point-code objects are compared with the expected ones and with those produced by parsing the
      equivalent legacy worker file;
  * conflict-rule tests for D-S1: both forms on one worker, mixed pool, file form in supervisor mode, transport
    commands outside worker-owned instances, `exit`/`end` lines, duplicate instance, inconsistent indentation. Each
    is refused with the offending line;
  * T10 extensions for the inline form; a VTY transcript test for the new command.
* **Existing tests:** all unchanged and passing; legacy E2E (file form) 46/46.
* **Acceptance:** all of the above; mutation per conflict rule and for the subtree rule. The BSC view of an inline
  configuration yields the same BSC objects as the equivalent legacy main file.
* **Failure cases:** view-builder/parser divergence (detected by the parse-validation tests; any divergence blocks
  acceptance).
* **As implemented (closed and accepted):** the BSC view is parsed in `aworker_pool_read_config()` (`aworker_pool.c`),
  called from `osmo_bsc_main.c`, not in `bsc_read_config()` itself. Option C follow-up and the dropped `route-table`
  filter: §12. The deprecation notice for the file form (SUP §4.5) is not implemented yet.

#### S2-2: snapshots, inline form in legacy mode

* **Objective:** in legacy mode, an inline pool gives each worker its view as a sealed memfd (fallback: unlinked file
  in a private directory). The worker parses the snapshot and never the file. The file form keeps re-reading its files
  exactly as today.
* **Files and functions:** `aworker_pool.c` `child_exec_worker()` / `worker_spawn()` (inherit the snapshot descriptor;
  for the inline form only, add a new option for the descriptor, while the existing `--aworker-config FILE` contract
  stays unchanged, N3); `aworker.c` `aworker_run()` / `cs7_collect()` (scan and parse from the descriptor);
  `osmo_bsc_main.c` option parsing; a new snapshot builder in `src/a-worker/`.
* **Tests to add:** process tests numbered from P21 (the fake worker gains the snapshot option; P1–P20 untouched):
  a worker reads only its snapshot; editing `osmo-bsc.cfg` after start does not change a restarted worker; the
  CS7_INFO ownership check is still active (mutation); memfd path and fallback path both tested (fallback forced).
  E2E variant with an inline configuration.
* **Existing tests:** P1–P20 (file form, unchanged argv), T1–T11, legacy E2E (file form).
* **Acceptance:** both legacy forms pass E2E; no file access by an inline-form worker (verified, e.g. by moving the
  file away after start).
* **Status:** all ten steps of the approved sequence done; the final verification (step 10) passed; accepted.
  Decisions, tests, results and limitations: §14.5.

#### S2-3: standalone `bsc-aworker` (no sessions)

* **Objective:** the real executable. It reads `osmo-bsc.cfg` (Supervisor view), registers the configuration
  commands it needs for parsing and **starts no runtime VTY server** (D-S6). It spawns workers by re-executing its own
  image in a worker role (own argv contract, N3), reaps them and applies the unchanged lifecycle policy. SIGTERM
  stops the pool cleanly with GOODBYE. No BSC attaches yet; workers stay with SSN 254 unbound. Controlled by signals
  only.
* **Files and functions:**
  * `src/a-worker/Makefile.am`: `bin_PROGRAMS = bsc-aworker`;
  * new `src/a-worker/aworker_main.c`: options, logging with the shared categories (D-S10), `vty_init()` with a
    `go_parent_cb` delegating to `osmo_ss7_vty_go_parent()`, `osmo_ss7_init()`, `osmo_ss7_vty_init_asp()`,
    `osmo_sccp_vty_init()`, `aworker_pool_vty_init()`, and **no** `telnet_init*()` call; event loop; signals;
  * `aworker_pool.c`: `aworker_pool_set_exec()` with a worker-role argv for the new binary; `worker_apply_policy()`
    plus a configuration check that refuses `shutdown` in supervisor mode (D-S8);
  * **header split (N4):** `aworker_pool.h` becomes a lifecycle header plus a BSC-specific user header, so the new
    binary no longer includes `bsc_sccp_backend.h`.
* **Tests to add:** a process-test suite launching the real `bsc-aworker`:
  * Supervisor SIGKILL terminates all workers (P2/P3 semantics with the Supervisor as parent);
  * workers run the Supervisor's own image (P4 semantics);
  * backoff, restart limit, FAILED, startup timeout, pool stop with GOODBYE (exit 0);
  * **no listening TCP socket** opened by the Supervisor (clarification B: checked from `/proc/<pid>/net` / socket
    inodes);
  * the executable's undefined symbols are only Osmocom libraries and libc (`nm`);
  * `osmo-bsc --vty-ref-xml` and `--help` unchanged.
* **Existing tests:** P1–P20 and T1–T11 against the library, unchanged.
* **Acceptance:** all of the above; logging per D-S10 (decision B2) including a durable concurrent shared-file
  regression test; no claim for `wq` mode unless tested.
* **Decided at the S2-3 design review (B1–B5, §14.2):** the name `bsc-aworker` (`bin_PROGRAMS = bsc-aworker`); the
  worker role reuses the S2-2 contract `--aworker-fd`, `--aworker-index`, `--aworker-snapshot-fd` with strict
  validation (index 0–15 as digits), hidden from `bsc-aworker`'s public help; identity prefixes only in the new
  executable. S2-3 starts only after S2-2 is accepted (its final verification, step 10, has passed) and S2-3 is authorized.
* **Status:** implemented and verified (stages 1–5 and a final verification); record, tests, results and
  limitations: §14.6.

#### S2-4: IPC v2 and fd-passing primitives

* **Objective:** major/minor version, boot nonce, incarnation and session_id in HELLO; message classes per channel
  (control, data, session); SCM_RIGHTS on the existing link.
* **Files:** `aworker_ipc.c/.h`, `aworker_ipc_link.c/.h`. Legacy use is transparent.
* **FD ownership rules (clarification A), implemented and tested here:**
  * at most one descriptor per message; receive buffer `CMSG_SPACE(sizeof(int))` via `osmo_iofd_set_cmsg_size()`;
  * **sender:** keeps its descriptor until `sendmsg_cb` reports completion, then closes it; if the link is freed with
    the record still queued, closes it then (`osmo_iofd_sendmsg_msgb()` copies only the descriptor number at enqueue
    time [V]);
  * **receiver:** `MSG_CTRUNC` → close every received descriptor, then the link; wrong descriptor count or a
    descriptor on a message type that takes none → close all received descriptors, then the link; `FD_CLOEXEC`
    set immediately (no `MSG_CMSG_CLOEXEC` in the pinned osmo_io [V]); every received descriptor has exactly one
    owner or is closed on every failure path;
  * partially received transfers (a record without its descriptor, or the reverse) are protocol errors handled as
    above.
* **Tests to add:** `aworker_ipc` unit tests: descriptor arrives intact and refers to the same open file; each failure
  case leaks nothing (descriptor counts of the process before/after, as Phase 1 P1 does); a descriptor queued on a
  link that is freed is closed; version and class rules.
* **Existing tests:** P1–P20, T1–T11 unchanged (exact-version test updated only for the documented version field
  change, stated in the commit).
* **Status:** implemented and verified; record, tests and limitations: §14.7.

#### S2-5: sessions, control/data split, operator channel, fencing, canonical document

* **Objective:** the worker's single link becomes a control link (Supervisor, worker lifetime; loss = worker exit)
  and a data link per BSC session (loss = detach). Supervisor session socket, session state machine (CLOSING entered
  exactly once, SUP §9.1), ATTACH/DETACH (minimal detach: close the data link, unbind SSN 254), WORKER_UP/DOWN/STATE,
  session ECHO, operator control channel (D-S6, N5), fencing via pidfd, canonical document comparison (CD-1, D-S5).
* **Files:** `aworker.c` (two links), `aworker_pool.c` (control link), new `src/a-worker/aworker_session.c`, operator
  client mode in `aworker_main.c`.
* **Endpoint ownership (clarification A), to be implemented exactly:**

| Endpoint | Created by | Held by | Closed by / when |
|---|---|---|---|
| Control socketpair, Supervisor end | Supervisor at spawn | Supervisor (link) | Supervisor on worker loss or pool stop |
| Control socketpair, worker end | Supervisor at spawn | worker only (the Supervisor's copy is closed in the parent right after `fork()`, as today) | worker exit |
| Data socketpair, worker end | Supervisor per attach | **worker only** after ATTACH is sent | Supervisor's copy closed in `sendmsg_cb`; worker on DETACH / session end |
| Data socketpair, BSC end | Supervisor per attach | **BSC only** after WORKER_UP is sent | Supervisor's copy closed in `sendmsg_cb`; BSC on session end or death (kernel) |
| Session socket (BSC side) | BSC `connect()` | BSC | BSC exit or death |
| Session socket (Supervisor side) | `accept()` | Supervisor | CLOSING entry action |

  * **EOF propagation:** the worker sees EOF on its data endpoint after a BSC death **only if no other process holds a
    duplicate of the BSC end.** Therefore: the Supervisor must have closed its copy of the BSC end (send completed)
    before the session is ACTIVE for that worker; no data endpoint may be inherited by any other child (workers are
    spawned with `child_close_fds()` keeping only their own descriptors [V]; data socketpairs are created
    `SOCK_CLOEXEC`); the BSC must not fork with the endpoint open. The Supervisor additionally sends DETACH on session
    EOF, so the worker detaches even if an EOF were masked by a stray duplicate, but the EOF itself is required and
    tested.
* **Tests to add:**
  * session tests with a scripted BSC client process: accept; reject for busy, version and config mismatch;
    UNRESPONSIVE never admits a second BSC; every race in SUP §9.1; exactly-once CLOSING (mutation: duplicate
    teardown detected); stale session_id or incarnation rejected; fencing with confirmed exit admits, unconfirmed or
    unavailable pidfd refuses; worker ECHO loss affects only that worker;
  * **real process-level EOF test (clarification A):** the scripted BSC client is SIGKILLed; each worker observes EOF
    on its data endpoint within a bound (logged and reported in STATE), independently of the Supervisor's DETACH
    (a test hook delays DETACH to separate the two causes); afterwards no process other than the worker holds a
    descriptor to the worker's data endpoint (checked via `/proc/*/fd` socket inodes);
  * descriptor accounting in the Supervisor across 100 attach/detach cycles (no growth);
  * operator channel: unauthorized client rejected (per N5), operator cannot change configuration.
* **Acceptance:** all of the above; CD-1, D-S12 and N5 resolved before this subphase starts.

#### S2-6: detached-worker release

* **Objective, tests, acceptance:** §13.4 in full (protocol test peer first; API acceptance, wire transmission, peer
  response and local removal recorded separately). D-S9 decided from the measurements before sign-off; no default is
  invented before that.

#### S2-7: BSC session client (opt-in supervisor mode)

* **Objective:** `osmo-bsc` with `mode supervisor` never spawns, signals or restarts workers; connects to the
  Supervisor (retry with backoff); sends SESSION_OPEN with its canonical document; exits on reject; runs
  `aworker_users.c` over data links; SESSION_CLOSE on planned stop (no GOODBYE); RESET via `a_worker_available()`.
* **Files:** `src/osmo-bsc/aworker_users.c` (data-link source), new `src/osmo-bsc/aworker_client.c`, `osmo_bsc_main.c`
  (mode switch; no `aworker_pool_start()` in supervisor mode).
* **Tests to add:** T-semantics over the session path; real-BSC process test: BSC SIGKILL → worker data-endpoint EOF
  observed (clarification A, with the real `osmo-bsc`), Supervisor and worker PIDs and incarnations unchanged;
  reattach rebinds and RESET is sent; legacy mode unchanged.
* **Transport survival evidence (clarification D),** recorded before the kill and after the reattach, and compared:

| Item | How it is recorded |
|---|---|
| worker PID | Supervisor STATUS, `/proc` |
| worker incarnation | Supervisor STATUS, WORKER_UP |
| SCTP association identity and state | local/remote addresses and ports and association state of the worker's SCTP socket (socket inode in `/proc/<pid>/fd`, kernel SCTP association table), unchanged inode and endpoints |
| M3UA ASP/AS state | STP VTY (`show cs7 instance … asp`, `… as`) and worker log: ACTIVE throughout, no state change logged |
| CS7/SCCP readiness | worker STATE (READY, CS7_INFO unchanged); SSN 254 rebound after reattach |
| worker restarts | Supervisor `starts` counter and restart record unchanged; no spawn logged |

  A surviving Supervisor PID alone is not accepted as evidence.

#### S2-8: E2E acceptance

* §13.5 (16 steps, three consecutive runs) with the clarification D evidence table recorded at steps 5–11, using the
  external harness extended for both binaries.

### 14.4 Open decisions and deadlines

| ID | Question | Current position | Must be resolved before | Required content / evidence |
|---|---|---|---|---|
| **CD-1** | Exact contents of the canonical configuration document (D-S5 contents; D-S3/D-S4 detail) | proposal SUP §4.6 (H/P/C); C is syntactic, not effective (see SUP §4.6 note) | **S2-5** | explicit values; effective defaults (explicit in P; how defaults of cs7 commands are represented in C); normalized ordering and representation; per-worker cs7 and transport configuration; pool and Supervisor settings; treatment of semantically identical but differently formatted or spelled configurations. Raw-file comparison is **not** acceptable. |
| **D-S12** | Session ECHO keys and values | keys proposed, values open | **S2-5** (keys and initial values); values confirmed by S2-8 measurements | interval and miss threshold; state transitions per SUP §9.2 |
| **N5** | Session/operator socket security | open | **S2-5** | socket directory and file ownership and mode; peer authorization (`SO_PEERCRED` uid/gid policy, session vs operator role); stale socket handling (only after acquiring the lock, never while another Supervisor holds it); lock semantics (`flock` exclusive non-blocking on a lock file for the Supervisor's lifetime; path); rejection of unauthorized local clients before any processing; abstract-namespace sockets excluded or justified. umask alone is **not** a sufficient policy. |
| D-S10 | Logging | **resolved by B2** (SUP §7), with limitations; the durable test exists since S2-3 (`tests/aworker_logfile`) | — | `wq` mode unverified, no claim made |
| D-S3 | Config mismatch → BSC exits | approved direction, detail proposed | S2-7 (behaviour); CD-1 first | — |
| D-S9 | Detach release timeout | **unresolved** | S2-6 sign-off | §13.4 measurements |
| D-S7 | Supervisor TCP port | deferred | only if a TCP interface is ever added | — |

### 14.5 S2-2 implementation record (steps 1–10)

**Approved decisions** (second S2-2 review, implementation authorization, and the final decisions of 2026-11-17):

| ID | Decision | As implemented / verified |
|---|---|---|
| D-1 | Snapshots built by the legacy osmo-bsc pool parent; code reusable by the S2-3 Supervisor | `aworker_snapshot.c` and `aworker_cfg_view.c` in `libaworker.la`, no BSC-only symbols (`nm` check, step 4) |
| D-2 | Worker views omit the `log file` section (with its scoped directives, and `no log file`); other logging kept; workers log to stderr | view rule + refusal of any worker view with `log file` (step 2); real binary: no worker descriptor of the BSC's log file, worker lines on stderr only (R-4) |
| D-3 | Worker reads with `pread()` from offset 0 | S-9, P24 (offset at the end, two restarts read everything) |
| D-4 | Fallback only without a sealed memfd; never called equivalent; `$RUNTIME_DIRECTORY` (first, absolute), `/dev/shm`, `/tmp`; directory accepted if owned by the effective user (any mode) or root-owned and sticky (final decision 1); all operations through the directory fd; leftovers reported, never deleted; same-user limit documented | S-3 … S-6, P28 (no usable directory: no fork), P29 (changed fallback refused before every start) |
| D-5 | 1 MiB per snapshot (1,048,576 accepted, one more refused, empty refused) | S-7 |
| D-6 | `--aworker-snapshot-fd N`: N > 2, ≠ `--aworker-fd`, needs `--aworker-fd`, not with `--aworker-config`; strict index 0–15 in this mode only (final decision 5); violations exit 2; N3 unchanged | R-1 (unit test against the real binary and `real_s22.py`) |
| D-7 | A reset reuses the startup snapshot; a configuration change needs an osmo-bsc restart; logged | P25 |
| D-8 | `show startup-config` in inline mode | was `fopen(NULL)` ("Can't open configuration file [(null)]"); fixed by recording the path after the BSC view parsed (`b4d890241`); R-3: shows the file read, also after a refused `write file PATH`; R-3b: file form and no pool unchanged |
| D-9 | Real-binary C8 | R-2: every VTY write form and CTRL `write-config-file` refused, file unchanged, nothing written |
| D-10 | No download; document the missing official transcript runner | scratch runner `vtyrun.py` (see limitations) |
| D-11 | Inline variant of the external E2E | `run_e2e.py --inline`: 49/49 |
| D-12 | Verify `MFD_NOEXEC_SEAL` and sealing here; unsupported flags handled without weaker seals | S-1, S-2 (EINVAL retry keeps all four seals), S-10: kernel 7.0.0-43, `vm.memfd_noexec` 0, `MFD_NOEXEC_SEAL` accepted, seals 0x2f |
| D-13 | Worker view checked as text before any parse | step 3 and the worker in step 6: refusals exit 66 with no parse, ASP or socket (child test; strace on the real binary, R-7) |
| SR / U-1, U-2 | Pool configurations read once (≤ 16 MiB); model, BSC parse, BSC view and all snapshots from that buffer; the path never reopened; metadata and second-pass comparison; best effort, not atomic | V-5, V-6 (step 1); R-6 (inotify: no open of the file during restarts) |
| FD | Child keeps stdio, its IPC socket, its own snapshot (and the close-on-exec exec fd until exec); worker closes the snapshot right after the read; worker i cannot reach j; cleanup after fork failure, exec failure, exit, restart | P21, P22 (write/truncate through `/proc` → EPERM), P27, S-8, R-5 |
| U-3 | `show startup-config` an acceptance gate, normal and ASan | D-8 above |
| U-4 | Supervisor logging policy in S2-3 | settled by B2 (§14.2) |
| U-5 | Mutation runner runs real-binary checks through a deterministic external command | `mutate.py` `"commands"` (harness backed up first); existing behaviour unchanged (byte-identical output on an old spec) |
| U-6 | `RLIMIT_NPROC` test only as non-root, else NOT EXECUTED | executed as uid 1000 (P27) |
| Names | `memfd:bsc-aworker-<i>`, `bsc-aworker-snap.<random>`, executable `bsc-aworker` (final decision 1, B3) | in code and tests; the T9 fixture string `/etc/osmocom/osmo-bsc-aworker0.cfg` deliberately unchanged (final decision 6) |

**Steps** (the approved order; each step was reported and accepted before the next one was authorized):

| Step | Content | Main tests | Mutations (caught / run) |
|---|---|---|---|
| 1 | single-read source (`aworker_cfg_read_stable()`, `aworker_cfg_model_build_buf()`) | V-5, V-6, D4/D5 cases | 16/16 (SR5 detected only by the call-sequence assertion; kept by decision) |
| 2 | D-2 worker logging rule | Part E (E1–E4) | 16/16 after the approved removal of the redundant L15 exclusion |
| 3 | D-13 worker-view check | Part F | 23/23 (V23 caught by the golden output only) |
| 4 | sealed memfd snapshots | S-1, S-2, S-7 … S-10 | 30/31 (N10: an ignored `F_ADD_SEALS` failure is caught by the `F_GET_SEALS` check) |
| 5 | secure fallback | S-3 … S-6 | 41/43 (F8/F9 redundant pair, accepted; F45 removing both is caught; F29 excluded) |
| 6 | worker snapshot mode, `--aworker-snapshot-fd` | `tests/aworker_worker` (real `aworker_run()` in a child; R-1 against the real binary) | 22/22 |
| 7 | pool parent integration and fd isolation | P21 … P30, T12 update | 22/22 |
| 8 | real-binary checks, inline E2E, scratch runner, U-5, D-8 fix | `real_s22.py` R-1 … R-7 + R-3b; `run_e2e.py --inline`; `vtyrun.py` | 8/8 real-binary (C8 ×3, D-8, D-2, R-5, R-7 ×2) |

**Results at the end of step 8** (fresh normal and ASan/UBSan builds): `make check` 19/19 in both; every A-worker test
run directly matches its `.ok`; no sanitizer report from any process (tests, forked fake workers, real osmo-bsc and
its workers); `real_s22.py` 72/72 normal, 73/73 ASan; E2E legacy 46/46, inline 49/49; `vtyrun.py` 5/5 transcripts
(`aworker_pool.vty` and four upstream ones) in both builds. Evidence: `~/dev/osmo-bsc-a-worker-recheck/step*/`.

**Known limitations and open points:**
* The fallback is weaker than a sealed memfd (same-user processes; root) — by design, logged as a WARNING.
* The single read detects changes during the read on a best-effort basis; it is not an atomic filesystem snapshot.
* D-10: the official `osmo_verify_transcript_vty.py` is not installed; `aworker_pool.vty` has been run only with the
  scratch runner `vtyrun.py`, which is not equivalent (documented in its header: subset of the matching rules, VTY on
  127.0.41.1 instead of 127.0.0.1, no update mode).
* The external harness (`real_s22.py`, `vtyrun.py`, `run_e2e.py`, `mutate.py` in `~/dev/osmo-bsc-a-worker-pool-e2e/`)
  is not in the repository.
* `real_s22.py` runs without STP or MSC (associations never come up); end-to-end signalling of the inline form is
  covered by the inline E2E (one host, two workers).
* `show startup-config` shows the file as it is on disk now; after an edit it can differ from the running snapshots.
* `show a-worker-pool` prints "config file (none)" for inline workers.
* The file-form deprecation notice (SUP §4.5) is not implemented.
* Moving a snapshot descriptor above fd 2 (osmo-bsc started with a standard stream closed) is not exercised by a test.
* P1–P20 contain pre-existing unguarded `kill(winfo(..).pid, …)` calls; with a worker that has no process (pid 0)
  they would signal the test's process group. Only reachable under mutants; the new P21–P30 guard it.
* Runtime configuration input is not checked against the pool rules: the BSC's interactive `configure terminal` and
  CTRL `apply-config-file` can still create an ASP in the BSC process, in any pool mode (pre-existing since Phase 1;
  the checks run at startup only). Not changed in S2-2.
* In snapshot mode a parse error is logged as "cannot parse the worker config file 'configuration snapshot (fd N)'"
  (cosmetic: there is no file, and the descriptor is already closed then).
* D-S10 (B2): `wq` mode unverified; the durable shared-file regression test belongs to S2-3.

**Step 10, final verification (2026-11-17/18, HEAD `b48979738`):**
* Clean builds of exactly the committed tree (`git archive`, autoreconf, the same configure options as before), normal
  and ASan/UBSan: no compiler warning in A-worker code (the only warning, in `neighbor_ident.c`, is upstream and
  untouched); `make check` 19/19 in both.
* Every A-worker test run directly, three times each, in both builds: identical to its `.ok`; no sanitizer report from
  any process (per-process sanitizer logs, forked and exec'd workers included).
* Real binary from the clean builds: `real_s22.py` 72/72 normal (identical to the step-8 golden) and 73/73 ASan;
  E2E legacy 46/46 and inline 49/49; `vtyrun.py` 5/5 transcripts in both builds.
* Mutations, every step's final spec on the final code: 177 of 180 caught (step 1: 16/16, 2: 16/16, 3: 23/23,
  4: 30/31, 5: 40/42, 6: 22/22, 7: 22/22, 8: 8/8). Survivors N10, F8, F9 are the accepted ones; F29 and F24 are
  excluded because they create or delete directories in the real `/dev/shm` and `/tmp`. Superseded spec versions
  (before the step-5 adaptations, L16) no longer apply to the code and were not run.
* No leftover processes, listeners or snapshot directories; the worktree equals the commit.

**Incidents during S2-2** (details in the step evidence under `~/dev/osmo-bsc-a-worker-recheck/`):
* Step 1: `make -n -B` re-ran autotools and reset `.deps` files in the old build directories; all later work used fresh
  build directories, the old ones were not used or changed again.
* Step 5: mutation F29 left four private directories in the real `/dev/shm` and `/tmp`, and mutation F24 then deleted
  them although they were to be preserved (a recorded preservation deviation; no stand-ins were recreated).
* Step 7: a new test helper sent `kill(0, SIGKILL)` for a worker without a process under a mutant, killing the mutation
  driver; the mutated source was restored by hand and verified, the helper guarded, the run repeated.
* 2026-11-17 16:57 and overnight: host reboots. `/tmp` is emptied at boot (tmpfiles rule), so the 641 preserved
  `/tmp/aworker_*` test directories were lost (not recoverable); the interrupted step-10 mutation run left one mutant
  in the worktree, restored from the driver's on-disk copy (identical to the commit) before resuming.

### 14.6 S2-3 implementation record (standalone Supervisor `bsc-aworker`)

**Scope as implemented:** the approved S2-3 objective (§14.3) with decisions B1–B5 (§14.2): the program `bsc-aworker`
runs the pool on its own, from `osmo-bsc.cfg`, with no BSC attached and controlled by signals only. Not in S2-3, and not
implemented: sessions and fd passing (S2-4, S2-5), the operator channel and its socket security (S2-5, N5), the
detached worker (S2-6), the BSC session client and supervisor mode in osmo-bsc (S2-7).

**Stages** (each verified with normal and ASan/UBSan builds, all suites, and a mutation run of its own, then committed):

| Stage | Content | Commit | Mutations (caught / run) |
|---|---|---|---|
| 1 | N4: the SCCP-user API moved unchanged to `aworker_sccp_user.h`; `aworker_pool.h` and `libaworker` without `bsc_sccp_backend.h`; no behaviour change (all outputs, osmo-bsc `--help` and `--vty-ref-xml` byte-identical) | `2cd86852c` | — (no behaviour) |
| — | test fix: P21 first collects orphans of earlier tests (an ASan-only race, 1 in 8 runs; 20/20 afterwards) | `f1c98eb23` | — |
| 2 | `aworker_pool_supervisor_read_config()`: single read; `a-worker-pool` with ≥ 1 worker, every worker inline (D-S1); the Supervisor view only (log targets, `a-worker-pool`); no `on-worker-failure shutdown` (D-S8); new test `tests/aworker_supervisor` | `f04ce1041` | 10/10 |
| 3 | `bsc-aworker` (`aworker_main.c`): Supervisor role (pool, signals, no server, exit codes 0/1/2) and the hidden worker role (B4); linked against `libaworker` and the Osmocom libraries only; new real-binary test `aworker_supervisor_proc_test` | `8dd320179` | 14/15 (A8 equivalent: the pool's `atexit()` handler stops it) |
| 4 | D-S10/B5: identity prefixes and the program name in `bsc-aworker` only (`--aworker-incarnation`, Supervisor only); FAILED message without the VTY reset hint in the Supervisor; durable shared-file test `tests/aworker_logfile` | `8728fb0d5` | 11/11 |
| 5 | documentation (this record, SUP status and as-implemented notes) | (this revision) | — |

**Tests** (`make check`, 22 tests): `aworker_supervisor` (configuration rules, each in a child process),
`aworker_supervisor_proc_test` (the real binary: options and worker-role options, configuration errors, workers READY
as children running the Supervisor's own image with only their snapshot option, the Supervisor holding the two sealed
snapshots and no network socket, restart from the original snapshot after the file was moved and replaced -- inotify:
never reopened --, restart limit and FAILED, SIGTERM with GOODBYE and exit 0, SIGKILL of the Supervisor ending its
workers, the executable replaced on disk, identity prefixes, the `log file` target kept by the Supervisor and not
opened by the workers), `aworker_logfile` (D-S10). Linkage: no BSC symbol defined; every undefined symbol resolves in
libosmocore, libosmovty, libosmo-sigtran, talloc or libc. Regression: P1–P30, T1–T12 and every golden unchanged;
osmo-bsc `--help`, `--vty-ref-xml` and its real-binary checks (`real_s22.py`) unchanged.

**Known limitations:**
* No BSC attaches yet: osmo-bsc and `bsc-aworker` must not run the same inline pool at the same time (SUP status
  note); the workers leave SSN 254 unbound.
* No reset or status command in the Supervisor before the operator channel (S2-5); a FAILED worker needs a restart of
  `bsc-aworker`. Control is by signals only.
* The per-start startup timeout is not exercised with the real binary (a real worker becomes READY right after its
  parse; it cannot be held in STARTING without a test seam). The same library code is tested by P12, P17–P20.
* `--aworker-incarnation` extends the B4 worker-role contract (internal, `bsc-aworker` only, strictly validated); the
  B4 options themselves are unchanged.
* osmo-bsc's `logging filter imsi` is accepted by `bsc-aworker` without effect.
* `wq` log mode untested (B2); packaging, systemd units and installation layout are out of scope (SUP §15).
* The file-form deprecation notice (SUP §4.5) is still not implemented (legacy mode, not S2-3).

### 14.7 S2-4 implementation record (IPC protocol 2.0, descriptor passing)

**Scope as implemented:** the S2-4 objective (§14.3): protocol versions, identity, message classes and SCM_RIGHTS on
the existing link, in `aworker_ipc.c/.h` and `aworker_ipc_link.c/.h`, plus the minimal use that makes them checked
today (the pool asserts boot nonce and incarnation; a `bsc-aworker` worker its incarnation). Not in S2-4: sessions,
the control/data link split per session, WORKER_UP and SESSION_* messages, the session socket, and anything that sends
ATTACH or DETACH (S2-5).

| Stage | Content | Commit | Mutations (caught / run) |
|---|---|---|---|
| 1 | protocol 2.0: header version = major (2); HELLO 2.0 extension (minor, boot nonce, incarnation, session_id); minor rule; the link refuses contradicting identities; the pool and `bsc-aworker` workers assert theirs. Documented test change: version 2 valid, HELLO 20 octets longer | `d162723bc` | 11/11 (one after adding the incarnation-mismatch case to `aworker_worker`) |
| 2 | message classes LINK / CONTROL / DATA / SESSION; per-link class set (default CONTROL + DATA, legacy unchanged); wrong class refused on send, closes on receipt | `20db5779d` | 6/6 |
| 3 | SCM_RIGHTS: ATTACH (one descriptor) and DETACH; `aworker_ipc_link_send_fd()`, `_take_fd()`, `cfg->fd_passing`; the sender and receiver ownership rules of clarification A | `b1e8e877e` | 15/15 (one after adding the full-queue case) |
| 4 | documentation (this record, SUP §8 as-implemented notes) | (this revision) | — |

**Tests** (`tests/aworker_ipc`): V1–V6 (identity round trip, minor rules, reserved fields, the link's comparison), K1–K4
(every type in exactly one class; wrong class on send and on receipt; default link; ECHO on a restricted link),
F1–F6 (a descriptor arrives as the same open file, close-on-exec; the sender's copy closed after the send; an untaken
one closed after `rx_cb`; refused sends; queued then freed, queued then closed, queue full, closed link; on receipt:
no descriptor, a descriptor on a type that takes none, two, three (`MSG_CTRUNC`), none expected by the link
(`MSG_CTRUNC`), a malformed record, a bad sequence number, before HELLO -- every case checked against the process'
descriptor count; the ATTACH/DETACH codec). `tests/aworker_worker`: a worker asserting another incarnation than its
main process is refused by both. Unchanged: P1–P30, T1–T12, `aworker_supervisor` tests and `real_s22.py`.

**Known limitations:**
* The legacy and inline E2E (`run_e2e.py`) and the VTY transcripts (`vtyrun.py`) could not run on the host while an
  Osmocom lab there held the same fixed ports on wildcard addresses (0.0.0.0:3002 …; a bind of 127.0.40.x to the
  same port is refused then). They were run afterwards, unchanged, in a private network namespace
  (`busybox unshare -r -n`, own loopback; the lab untouched) against the clean builds below: E2E legacy 46/46,
  inline 49/49; VTY transcripts 5/5 on the normal and the ASan build, 0 sanitizer reports; results as in S2-3.
* **Final verification** (clean `git archive` builds of `8ec333b34`, normal and ASan): `make check` 22/22 on both;
  every A-worker test run directly 3× on both builds, output identical to its golden, 0 sanitizer reports from any
  process; `real_s22.py` PASS on both (normal byte-identical to the S2-2 golden); `osmo-bsc --help` and
  `--vty-ref-xml` identical to S2-3; `bsc-aworker` links no BSC code; harness files and the old build directories
  unchanged.
* ATTACH and DETACH are defined but not sent or handled by the pool or the worker until S2-5; a worker receiving one
  today ignores it (the link closes an untaken descriptor).
* The boot nonce is random per pool start of either program; osmo-bsc's workers assert no identity (legacy).
* Foreign control messages other than SCM_RIGHTS are handled (link closed, descriptors closed) but not exercised by a
  test: an AF_UNIX peer cannot send another type without the receiver enabling it.


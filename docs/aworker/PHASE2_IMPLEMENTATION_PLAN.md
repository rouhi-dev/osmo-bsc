# A-worker pool, Phase 2: implementation plan, task breakdown, acceptance criteria

Status: **approved** (with the clarifications in §11); implementation in progress. Architecture: `PHASE2_ARCHITECTURE.md` (approved with the decisions
below). Baseline: `a-worker-pool` at `e54578f2c` (Phase 1).

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

# Managed routine shutdown and durable safety state

[Owner contract](../../interfaces/simulator-stop.md) · [Dispatcher](dispatcher.md) · [M5 qualification](../../../island/implementation-todo.md)

## Problem and boundary

The R06 selected-child receipt proves that EOS stopped both MuJoCo actuators, drained and reaped its direct child group, and removed its sockets before IsOS replaces the run. The current `StopGuard` calls `Dispatcher::emergency_stop()` for every routine IsOS Stop or Restart. That call also persists `estop_latched=true` in the journal. A fresh EOS process therefore correctly advertises `base.velocity` and `arm.movej` as unavailable, even after IsOS has a confirmed stop receipt. The selected Q01 replay exposed this at the EGIA planner catalog; increasing its wait or clearing the journal in a test would hide the safety state.

The implementation will distinguish an owner-requested **routine managed shutdown** from a genuine `safety.estop`. It adds no public reset operation, graph field, grant, actuator ingress, or new wire identity. A genuine emergency latch remains durable and cannot be cleared by Stop, Restart, ordinary `robot.stop`, or a new simulator generation. A crash, unknown movement release, unknown physical stop, or unknown child reap remains fail-closed.

## State transition

1. Before any actuator stop, EOS closes movement and evidence admission and sets the Dispatcher in-memory inhibit. The managed routine path snapshots the admitted moving targets. The journal adds a `1.1.0` robot-bound `shutdown_pending` marker and selected `shutdown_targets`; pending is true exactly when targets are nonempty. The reader strictly verifies a `1.0.0` control record using its original body shape and checksum before assigning false/empty defaults; only the next successful control mutation upgrades the shape and checksum. A pending marker's targets must match the admitted robot/profile on reopen. This marker is distinct from the existing `robot_inhibited` recovery state and `estop_latched`; a new EOS process with an unresolved marker withholds movement. It never overwrites or treats a preexisting cause as its own.
2. It starts the existing one-request MuJoCo companion-socket stop coordinator within the R06 deadline, before waiting for journal persistence. Concurrent public `robot.stop` or `safety.estop` joins the physical request but retains its own durable identity and safety effect. The routine path atomically persists its own shutdown marker and target set before a positive physical-stop result can be claimed. If this write stalls or fails, the companion stop still runs; the selected outcome is unknown, without claiming that a marker was persisted. If EOS crashes before the write, IsOS's selected unknown-stop gate blocks replacement.
3. A fully matched generation and newer epoch must prove both base and arm stopped. EOS then drains/reaps the selected MuJoCo group, removes both backend sockets and its control socket, shuts down host skills and evidence, and checks every existing R06 receipt predicate and global deadline. Public ingress closes before stop admission ends; a `safety.estop` admitted before closure must durably latch and settle, while later calls are denied. Any in-flight public stop still unresolved at the deadline makes the selected receipt unknown. No subset of these facts permits a confirmed receipt. Internal shutdown's per-part result is evidenced by the selected raw receipt; only public stop operations have separate durable per-part journal records.
4. Only when those predicates are true and no public stop is still in flight may EOS durably fence terminal uncertain movement after the proved newer physical stop epoch, preserve its robot inhibit, and resolve **only this shutdown marker and its target list** under one journal lock. A nonterminal or legacy unresolved movement prevents resolution. A write failure makes the receipt unknown. This resolution never changes `robot_inhibited` or `estop_latched`; either may independently remain true. The current process remains quiesced and in-memory inhibited through exit. It never resumes movement. The selected raw stdout receipt is then published once; IsOS may start a new run only after its own strict receipt and successful EOS exit checks.
5. If any predicate is unknown, the shutdown marker remains durable. A late adapter result cannot clear it. If a genuine `safety.estop` was latched before or during shutdown, the latch remains true even if the routine physical stop and child reap were confirmed. A new EOS run advertises movement unavailable until a separately specified, authorized safety recovery exists.

Historical operation IDs and valid old control-record reads remain intact. A crash between setting and resolving the shutdown marker starts the next EOS generation inhibited. A crash after resolution but before a selected receipt is still blocked by IsOS's unknown dependent-stop ledger; no recovered log line can upgrade it. The existing old-run operation IDs remain query-only and cannot be resubmitted under a new run grant.

## Verification

Use fake-clock and blocking adapter tests to prove physical stop preempts a delayed movement, journal intent stalls cannot delay it, and a positive physical response alone never clears the fence. Test a preexisting estop and preexisting robot inhibit with an otherwise fully proved routine Stop: the stop receipt may confirm, but each independent safety state must still deny movement in the next run. A previously `Releasing` movement permits a confirmed stop only after the old physical release is fenced and its journal record durably resolved; its prior inhibit remains. If that movement is still unresolved at the deadline, the shutdown marker stays pending and the receipt is unknown. Test concurrent estop, journal write failures, partial dual-stop proof, child forced kill and receipt-write failure separately; each unproved stop or persistence failure must produce no confirmed receipt and preserve the required safety state. A real managed IsOS→EOS→MuJoCo Stop and explicit Restart replay must show an unchanged saved revision, confirmed old receipt and reap, a fresh child generation and grant, EGIA's ready exact planner binding, a fresh operation ID with one EOS journal terminal result, old-ID opacity, and final confirmed Stop. Preserve the complete logs and an offline tamper-rejecting verifier under `logs/test/<run-id>/`.

## Focused owner implementation evidence (2026-11-02)

The managed EOS subprocess test uses an isolated generated graph, run-scoped verifier and strict-socket simulator stub. Two confirmed routine Stops each reaped the child and left `.control` at `1.1.0` with `shutdown_pending:false` and `estop_latched:false`; the second owner generation reopened the same journal. A third partial dual-stop returned `unknown`, retained `shutdown_pending:true`, and did not create an estop latch. Dispatcher tests cover delayed movement, failed marker persistence/resolution, a transient admitted estop latch-write failure followed by a successful routine marker write, preexisting inhibits and estop, admitted public-stop serialization, and the durable fence on a terminal uncertain movement. A changed selected profile cannot claim to fence an old uncertain target absent from its shutdown proof. This is EOS owner evidence; the cross-owner IsOS Restart and EGIA planner replay remains a separate Q01 gate.

From this worktree root, the reproducible gates were:

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace -- --test-threads=1
cargo build --workspace
python3 scripts/check_doc_links.py docs/eos/interfaces/simulator-stop.md docs/eos/usage/mujoco.md docs_cn/eos/interfaces/simulator-stop_cn.md docs_cn/eos/usage/mujoco_cn.md
git diff --check
```

All passed; complete output is under `logs/test/20261102-managed-shutdown/`. No throughput or latency claim is made.

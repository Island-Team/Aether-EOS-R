# M3 governed execution qualification

[中文](../../docs_cn/island/m3-qualification_cn.md) · [Milestones](implementation-milestones.md) · [Detailed TODO](implementation-todo.md) · [EOS stop design](../eos/design/runtime/simulator-stop.md)

**Status: M3 qualified on the scoped governed-execution exit row (2026-11-02).** R01–R07 have reviewed owner evidence. The exact merged-pin first-use replay selected a real IsOS → EOS → MuJoCo graph for pick-place and navigation, and an EGIA typed client discovered active base/arm bindings and submitted six correlated operations across three cases. This closes the M3 exit row; it does not close M4's agent/workbench stages or M5's full product release run.

## Exact inputs and preserved evidence

| Owner | Source pin | Use in this run |
| --- | --- | --- |
| EOS | `2437f81a661be57406029015b63530c8ed6b898a` | Rebuilt runtime, bridge, config server and profile. |
| IsOS | `b5553ad9a25527f3481a4ff6e64aeb3b92c43497` | Rebuilt manager and clean-home owner path. |
| MuJoCo | `4e6b5449ccd25be53ad95b13baa2c89f7ed91e10` | Rebuilt linked `island/mujoco-sim@0.1.1` bundle. |
| IsosUI | `4029728994d5a53dac76f2a566534e0472c4fab6` | Source pin for guide review only; local `dist/local` was used, without a UI build or live UI qualification in this replay. |

The independent `reviewer-verdict.md` supports the original R06 checkbox and scoped M3 closure. The ignored primary EOS directory `logs/test/20261102-m3-exact-merged-replay/` has a checksum manifest covering **158 evidence files** (`evidence.sha256`; 159 directory files including the manifest). The reviewer first audited a 156-entry manifest; the final expanded manifest verifies 158/158 files. Its `qualification.md`, `commands.txt`, `source-pins.txt`, binary hashes, event order, case directories and full command logs are the reproducible record. The replay initially found a verifier assumption about P01's `local_link` path; the corrected verifier and all three live cases passed, and the initial harness traceback remains saved. The primary checkout's `temp_design.md` was untouched.

## Exit evidence and limits

R01's merged route implementation and current-pin route tests cover weighted fairness, frequency caps, bounded queues, overflow and stop fences. R02/R03's reviewed gRPC and Dispatcher records cover typed discovery, complete-generation readiness, role/revision/binding admission, fake-clock freshness, durable terminal/unknown journal semantics and restart non-retry. R04's reviewed host ABI, IsOS selection and checked retirement, plus current-pin negative tests, cover unconfigured and invalid-package failure before guest invocation, plus undeclared-capability denial by the Broker before Dispatcher or physical release. R05's reviewed strict socket and binding validation, deduplication and health proof support the real child. R07's separately sealed browser bridge and first UI primitive demo evidence covers its owner row; this replay did not repeat UI behavior. The [detailed TODO](implementation-todo.md#verification-record) links those owner records.

The current-pin replay ran fresh homes through package link, scene/node selection, Validate and revision-checked Apply, launch and authoritative ready status. EGIA typed discovery found `base.velocity` and `arm.movej`; all six admitted operation IDs matched `Completed` EOS journal entries. Pick-place and navigation selected raw Stop receipts confirmed base and arm stop, admission closure, backend group drain/reap and socket removal. A lost companion stop socket produced `eos_stop_unconfirmed` and an unknown raw receipt; manager restart retained the degraded old run and rejected both Restart and Start without a new deployment or journal replay. Current-pin focused host skill, Dispatcher, route, IsOS selection/preflight and fixture tests passed. Sealed R06 exact-head and merged-input negatives cover child/provider death, forced stop, withheld response and wrong-pipe receipt; implementation source is unchanged at these merged pins. Those negative live cases were not all rerun in this combined replay.

The R06 checklist is checked against its **original literal owner acceptance**: closed admission, bounded physical stop lane and confirmation-or-unknown classification before child termination/reap, reverse dependent process stop, and the specified fault/cleanup tests across owner evidence. The broader live proof remains limited: physical partial-stop acknowledgement, manager death during an in-progress Stop, and a post-ready GUI render block across owners were not exercised end to end. The replay also did not qualify a full EGIA stage pipeline, browser UI, benchmark, upgrade/migration, or full M5 run. M4 and M5 remain open.

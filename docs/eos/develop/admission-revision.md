# R03 admission first-use and verification

[中文](../../../docs_cn/eos/develop/admission-revision_cn.md) · [Owner contract](../interfaces/grpc.md#r03-governed-actuator-admission-target-contract) · [Design](../design/runtime/admission-revision.md) · [Cases](../../../tests/fixtures/r03_admission_revision/README.md)

**R03 qualified after owner merges and post-merge replay.** EOS verifies the schema-2 generation and run-scoped grant; IsOS issues and delivers the run-scoped grants. The steps below are the reproducible first-use replay. R05/R06 separately qualify physical MuJoCo deduplication, fencing and stop/reap order; the subsequent [scoped M3 qualification](../../island/m3-qualification.md) closes the exit row.

1. Prepare a clean IsOS home and install the exact verified simulator package. Select a scene and the generic camera/base/manipulator nodes, then Validate and Apply the complete graph. Record the saved whole-graph revision from IsOS. The generated schema-2 snapshot must be at `data/generated/<deployment>/v2/<revision>/<generation>/`; verify `manifest.json` and the listed `eos/graph.json` bytes. A legacy `1.0.0` graph may be inspected and migrated explicitly, but its old layout must not authorize a movement.
2. Launch the managed EOS process through IsOS with the selected `/v2/<revision>/<generation>/eos/graph.json` profile, `EOS_CONTROL_GRANTS_FILE` verifier path and matching EOS `--run-id`. Confirm health `SERVING` only after full discovery, and read `eos-active-revision` and `eos-projection-sha256` response metadata separately. The first must equal IsOS's active saved-byte revision; the second identifies EOS's projection bytes and need not equal it. A missing, changed or mismatched verifier fails startup before readiness. An absent or changed EGIA/bridge client grant file does not by itself change EOS listener readiness; that client cannot authenticate with the missing or altered grant. A previously copied old secret may still match the verifier until the IsOS run rotates. An optional `--run-id` with a standalone non-v2 profile and no verifier remains read-only.
3. In a supervised EGIA or bridge process, obtain only its assigned grant path from the private launch environment. Send one bounded primitive with the exact discovered binding, role grant, saved active revision, typed arguments, command ID and deadline. Query `GetOperation` with the same grant and ID; compare the terminal acknowledgement command ID and primitive/version. If Submit times out or its reply is lost, query that ID before any new command. Never infer physical success from transport success, a graph wire, or a pending result.
4. Exercise a stale revision, wrong role, absent or changed client grant, bad binding, stale health/observation, numeric bound and expired deadline. Each must call the stub adapter zero times. Fill the movement lane and confirm authenticated `safety.estop` still enters its reserved lane with no current revision. During a RobotStop inhibit, confirm only an operator grant plus explicit recovery marker and fresh ID may attempt the union of old and current targets; it must not clear the emergency latch.
5. Read the current run's first failure or ready event through IsOS logs, then filter by `correlation_id` and command ID. Check that logs contain only sanitized role, revision and result codes, not grant bytes or movement arguments. Inject a journal fault and crash after `Releasing`; after restart `GetOperation` must retain `OUTCOME_UNKNOWN` and no adapter auto-retry. Cross-role Get/Watch/Cancel must fail without result leakage or cancellation.
6. Stop through the owner path and inspect terminal or uncertain outcomes. R03 proves Dispatcher admission and journal behavior with a stub; record R05/R06 physical stop and simulator termination evidence only when those separate gates run. A saved draft changed without Restart does not alter EOS's admitted revision. After Restart, rediscover before another command. Explicitly migrate a legacy graph with IsOS's revision-checked migration path.

Before review, run `python3 tests/fixtures/r03_admission_revision/check_cases.py`, `python3 scripts/check_doc_links.py --all`, the focused fake-clock and real-listener tests named in the case map, `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace`, and `cargo build --workspace`. Save complete output under `logs/test/<run-id>/` and record exact commands, environment, case-to-test mapping and independent review in this page. Do not make a performance claim until a fixed workload reports throughput, median and p99.

Design-only verification on 2026-11-02: `python3 tests/fixtures/r03_admission_revision/check_cases.py` validated 25 planned cases; `python3 -m py_compile tests/fixtures/r03_admission_revision/check_cases.py` passed; `python3 scripts/check_doc_links.py --all` checked 310 Markdown files with zero broken relative links; `git diff --check` passed. Complete output is in ignored `logs/test/20261102-r03-design/`. These commands check documentation and frozen case structure only; they do not qualify the R03 implementation or the IsOS grant owner slice.

## EOS topic evidence and R03 qualification

On 2026-11-02 the EOS `refactor(v2)/admission-revision` topic branch passed local checks on Linux x86-64 with Rust workspace version `0.2.0`. Run the commands below from the EOS worktree root; complete command output is retained in ignored `logs/test/20261102-r03-eos-integration/` (one log per command).

| Command | Evidence and result |
| --- | --- |
| `cargo fmt --all -- --check` | `fmt-check.log`; passed with no diff. |
| `cargo clippy --workspace --all-targets -- -D warnings` | `clippy-workspace.log`; passed. |
| `cargo test --workspace` | `test-workspace.log`; 50 test results, 251 passed, zero failed, one intentionally ignored live-MuJoCo test. |
| `cargo build --workspace` | `build-workspace.log`; passed. |
| `cargo test -p aether-runtime --test process -- --nocapture` | `logs/test/20261102-r03-process-boundary/process.log`; 15 passed, zero failed. |
| `cargo test -p aether-http-bridge --test bridge` | `bridge-tests-v3.log`; 5 passed, zero failed. |
| `cargo test -p aether-dispatcher` | `logs/test/20261102-r03-core-initial/dispatcher-test.log`; 39 unit, 18 integration and 15 gateway-stop tests passed, zero failed. The focused case mapping is in that directory's `case-map.md`. |

The EOS process tests cover read-only standalone discovery, pre-ready rejection for partial or altered managed inputs, selected schema-2 profile and verifier checks, active revision and projection digest discovery, role and revision refusal, a governed EGIA movement with a stub, and denied raw private routes. The bridge tests cover private operator-grant forwarding and browser rejection of caller-supplied grant. The existing P06 event stream reports `start_failed` before readiness for managed launch failures and emits `ready` only after startup; R03 does not add a command event schema. Durable command status and recovery remain in the Dispatcher journal and gRPC Operation projection, with role/run-scoped Get, Watch and Cancel. No grant bytes should enter logs or event payloads.

This table records EOS-only evidence with fixture-issued grants. The cross-repository replay and post-merge primary replay below check the real IsOS grant handoff. R05/R06 still own physical MuJoCo fencing and stop/reap proof.

### Final reviewed EOS code head

Independent implementation review reached EOS code commit `607eb0a93824c9488d17d38c9785c44f87ca4934` on 2026-11-02. The final exact-code run is in ignored `logs/test/20261102-r03-eos-final/`. From this EOS worktree root, run:

| Command | Complete output | Result |
| --- | --- | --- |
| `cargo fmt --all -- --check` | `fmt.log` | Passed. |
| `cargo clippy --workspace --all-targets -- -D warnings` | `clippy.log` | Passed. |
| `cargo test --workspace` | `test.log` | 50 test results, 252 passed, zero failed, one intentionally ignored live-MuJoCo test. |
| `cargo build --workspace` | `build.log` | Passed. |
| `cargo test -p aether-runtime --test process` | `process.log` | 16 passed, zero failed. |
| `cargo test -p aether-http-bridge --test bridge` | `bridge.log` | 5 passed, zero failed. |

The final process case verifies that another role's operation ID and an absent ID return the same `NOT_FOUND` response on managed Get. Code review confirms the shared managed lookup mapping also covers wrong-run and sealed legacy IDs on Get, Watch and Cancel; these additional combinations do not have a separate process assertion. The earlier historical unmanaged in-process lookup still reports `DATA_LOSS` for a legacy record without admission identity, preserving its recovery diagnostic without making that record writable in a managed run. The browser bridge reads and rechecks its private operator grant when forwarding `primitive_typed`; browser-provided grant bytes remain forbidden. The EOS owner slice has local review and exact-code gates. The cross-repository replay below passed locally; the later post-merge primary replay qualified R03.

### Local cross-repository replay

On 2026-11-02 a Linux x86-64 replay passed against IsOS topic commit `5c1cb27878d5594e9eda2f07765395e2830da045` and EOS code commit `607eb0a93824c9488d17d38c9785c44f87ca4934` with EOS documentation head `8df7e01e29d1ff609846cd7b56145a05ee868541`. The script and complete output are saved in the primary IsOS checkout at `/home/ashex/projects/isos/logs/test/20261102-r03-cross-repo/`; `r03-cross-replay.executed.py` records the exact candidate-worktree run; `r03-cross-replay.py` and `probe/src/main.rs` are the preserved primary-checkout replay sources, while `mock-base-grpc-pairwise.log` records the exact candidate run and `generalized-candidate-pairwise.log` records the generalized replay against the same candidate binaries.

From the primary IsOS checkout with the merged owner commits and rebuilt binaries, run:

```sh
cargo build --manifest-path logs/test/20261102-r03-cross-repo/probe/Cargo.toml --offline
ISOS_BIN=$PWD/target/debug/isos \
ISOS_AETHER_BIN_DIR=/home/ashex/projects/Island-eos/target/debug \
python3 logs/test/20261102-r03-cross-repo/r03-cross-replay.py
```

The preserved probe `Cargo.toml` points to the primary EOS checkout after merge. The separate `r03-cross-replay.executed.py` preserves the exact candidate-worktree paths used for the passing evidence. The replay creates its own temporary `ISOS_HOME` and does not print grant bytes.

The replay created a clean `ISOS_HOME`, used the exact selected EOS candidate binaries, and applied an IsOS three-process starter with EOS's `valid_bindings` mock base. It checked the selected V2 generation and active saved revision, private verifier modes and per-recipient environment isolation, then used real gRPC to discover the revision and projection digest, submit admitted base velocity, and recover its matching operation ID through Get. An operator grant could not submit gRPC movement, a stale revision failed, and another role's lookup matched the opaque missing-ID result. Explicit Restart kept the saved revision while rotating the run ID; the updated scripts assert that the EGIA grant and operator grant each changed individually. The new listener rejected the retired EGIA grant. Stop retired the active bundle after child stop. All pairwise grant and boundary assertions passed in `mock-base-grpc-pairwise.log` and `generalized-candidate-pairwise.log`. This candidate replay covers the managed EOS/IsOS boundary with a mock base. The post-merge primary replay below qualifies R03; R05/R06 separately qualify physical MuJoCo fencing and stop/reap order.

### Post-merge R03 qualification

EOS [PR #315](https://github.com/Island-Team/Island-eos/pull/315) merged into `refactor(v2)/runtime-main` at `dbd2e2c39a54236786577ace424c30e418ff3926`; IsOS [PR #52](https://github.com/Island-Team/isos/pull/52) merged into `master` at `df3c1f5692c9348c1cfb5b2ecb8b1abfa9fa82f2`. After rebuilding both primary checkouts, the probe build and generalized replay ran from `/home/ashex/projects/isos`: `cargo build --manifest-path logs/test/20261102-r03-cross-repo/probe/Cargo.toml --offline`, then `ISOS_BIN=$PWD/target/debug/isos ISOS_AETHER_BIN_DIR=/home/ashex/projects/Island-eos/target/debug python3 logs/test/20261102-r03-cross-repo/r03-cross-replay.py`. Complete probe build and replay output are in the primary IsOS checkout at `logs/test/20261102-r03-cross-repo/post-merge-probe-build.log` and `logs/test/20261102-r03-cross-repo/post-merge-primary.log`, respectively. The primary owner build outputs are EOS `logs/test/20261102-r03-eos-final/post-merge-build.log` and IsOS `logs/test/20261102-r03-control-grants-impl/post-merge-build.log`.

The clean-home replay passed selected V2 generation and recipient-isolated verifier handoff, real gRPC discovery and admitted base velocity/Get, wrong-role and stale-revision refusal, opaque operation lookup, pairwise EGIA/operator grant rotation, retired-grant rejection after Restart, and bundle retirement after Stop. This qualifies the R03 EOS/IsOS owner boundary on merged commits. The mock base does not prove MuJoCo physical epoch fencing or stop/reap ordering; R05/R06 and the scoped M3 exit were subsequently qualified on merged inputs; see the [M3 record](../../island/m3-qualification.md).

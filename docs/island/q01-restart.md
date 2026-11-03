# Q01 selected model fault and explicit Restart

This is the focused Q01 new-run failure/restart leg. The [cross-repository TODO](implementation-todo.md) keeps Q01 open until the complete install, selection, UI, negative-version and migration sequence is replayed together. The selected setup here links the verified MuJoCo package; it does not qualify a fresh package install.

## Owner contract

EGIA owns a model-driven mission and its durable terminal result. A successful model HTTP response containing an invalid controller command counts toward the selected `decision.max_invalid_replies` limit. The failed mission must create no proposal submission or EOS movement record. IsOS owns process-control Restart, stops and reaps the old selected deployment, then assigns a new run ID and new control grants. EOS owns the dispatcher journal and hides an old-run operation from the new grant. The graph bytes and active revision remain fixed. A new mission creates a distinct operation ID; it is not a retry of the old action.

The owning wire behavior is unchanged: [EOS gRPC capability contract](../eos/interfaces/grpc.md), IsOS `docs/isos/interfaces/control-grants.md` and `docs/isos/interfaces/dependent-stop.md`, and EGIA `docs/egia/interfaces/pipeline.md` and `docs/egia/interfaces/safety-reset.md` at the source pins recorded below. This page records the cross-owner replay predicate and does not introduce a new API field.

The model fault fixture returns `INVALID_Q01_REPLY` as an HTTP 200 completion only for controller requests. The sentinel lane retains its normal reply. It logs bounded request metadata and mode changes without prompts, authorization headers or grant contents. It is repaired only after the fault mission reaches terminal `failed` and the no-movement check passes.

## Replay design and evidence

`tests/m4_affected_replay.py --mission --q01-restart` constructs a private IsOS home, selects a pinned graph, validates and applies it, launches EOS/MuJoCo and EGIA, and records one initial supervised movement. It then arms the fault, records every parse failure and the owner limit event, verifies no EOS journal change, commands a governed robot Stop, and requests explicit IsOS Restart through the HTTP owner API. It records old/new status, process IDs, grant digests, revision, selected EGIA readiness, old-ID lookup and retired-grant rejection. A fresh supervised mission must produce one new terminal EOS journal record. The replay ends with a second governed robot Stop and confirmed IsOS Stop.

After Restart, the replay waits for the new EGIA process's northbound primitive catalog to expose a ready `base.velocity@robot-a/drive` binding. `egia_ready` alone does not establish planner visibility; an early mission can otherwise be rejected as an unknown primitive. The catalog frame is archived with the selected run ID and EGIA PID. Routed delivery is a later summary event and is not used as a readiness gate.

The journal records one admitted typed command and a terminal adapter acknowledgement. MuJoCo's strict socket retains deduplication state in memory but exposes no read-only execution-history endpoint; this archive does not independently count simulator executions. The existing R05 strict-socket tests cover same-ID deduplication.

`tests/verify_q01_restart.py <archive>` verifies the retained graph bytes, selected overlay, raw owner events, two missions, controller fault metadata, grant digests, run order, journal file hashes, and Stop status without the transient home. Change a retained fault attempt, run ID, operation record or hash to confirm tamper rejection. The archive contains digests but never grant bytes.

This slice does not claim same-run EGIA child replacement, lost-command same-ID settlement across a new grant, or full Q01 completion. The selected SIGKILL/latch negative and stable-run EGIA process restart proof are recorded separately in [M4 affected replay](m4-affected-replay.md).

## First-use command

Use exact source and binary pins from `selected-inputs.json` in the retained run. Replace checkout and binary paths with local copies at those recorded hashes. The full invocation and raw output are in ignored `logs/test/20261102-q01-restart/live-*.log`; run `python3 tests/verify_q01_restart.py <archive>` after completion. Read `q01-summary.json` for both run IDs and distinct operation IDs, `q01-planner-fault-events.json` for the bounded failure, and `q01-restart-response.json` plus `stop-response.json` for owner outcomes.

## Selected replay and checks

The qualifying scoped run is `logs/test/20261102-q01-restart/live-8/` on Linux x86_64, Intel i5-12490F, Rust 1.98.1. It selected EOS `1b850f56`, IsOS `2bdf6ea0`, EGIA `0c0c7670`, IsosUI `6afd5bdc` and the pinned MuJoCo release digest recorded in `selected-inputs.json`. The exact replay arguments are archived in `command-argv.json`; `live-8.log` ends with `Q01_RESTART_SCOPED_PASS`. The old/new run IDs and movement operation IDs differ; the final IsOS Stop is `confirmed`. The corrected finalizer captured the complete EGIA private log directly and the verifier checks its fault excerpt, all three raw `1.1.0` control checksums, planner catalog readiness before the fresh mission, journal identity and child reap.

From the EOS checkout, rerun `python3 tests/verify_q01_restart.py logs/test/20261102-q01-restart/live-8` and `PYTHONPATH=tests python3 -m unittest test_m4_model_stub`. The latter needs `PYTHONPATH=tests` because the test imports `m4_model_stub` as a top-level module. The complete gate outputs are `verify-live-8.log`, `model-stub-final-pass.log`, `fmt-final.log`, `clippy-final.log`, `workspace-test-final.log`, `build-final.log`, and `doc-links-final.log` in the same ignored test directory. Commands were `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace --all-targets -- --test-threads=1`, `cargo build --workspace`, and `python3 scripts/check_doc_links.py --all`; all passed. A hardlink copy with one altered raw control record was rejected by the archive verifier (`tamper-control-live-8.log`). This is scoped restart evidence; the complete clean-home Q01 sequence remains open.

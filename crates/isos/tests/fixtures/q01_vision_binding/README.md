# IsOS Vision binding Q01 cases

These frozen cases are **target design, not runtime evidence**. `cases.json` fixes positive and negative owner expectations for the first implementation tests. Run `python3 tests/fixtures/q01_vision_binding/check_cases.py` to validate case structure. The selected Q01 replay must later exercise the live route, identity, cap and queue counters; this fixture alone does not close Q01.

The IsOS owner implementation now covers selected, disabled, wrong source/gate, missing dependency, stale attestation, private grant mode, verifier tamper and Stop revocation in `src/runtime/vision_binding.rs` and `src/runtime/control_grants.rs` tests. `cases.json` remains a design matrix: it is not a selected EOS/EGIA live receipt. The cross-owner Q01 replay and remaining negative cases are tracked by the owning contract.

The short-endpoint cases cover a long real private home, both 107-byte socket preflights, per-run endpoint rotation, verifier tamper, revocation before child stop, cleanup after reap, and an orphan that blocks replacement. The endpoint directory is private run state under `run/v/`; grant files remain under `run/control-grants/`.

[中文](README_cn.md)

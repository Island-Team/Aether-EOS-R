# Verification

This page is for contributors checking the current IsOS tree. Save complete command output under ignored `logs/test/<run-id>/`; record the source commit and toolchain with each run.

```sh
cargo fmt --all -- --check
cargo clippy --all-targets -- -D warnings
XDG_CONFIG_HOME=/tmp/isos-generic-node-empty cargo test --all-targets -- --test-threads=1
python3 tools/check_isos_docs.py
python3 tests/fixtures/control_grants/check_cases.py
python3 tests/fixtures/reader_features/check_docs.py
git diff --check
```

The 2026-11-05 generic-node run passed formatting, strict Clippy, 324 unit tests and 24 integration tests; one owner-dependent test was ignored. Focused two-family tests also cover registered declarations, selector IDs, typed edges, changed resources, unsafe paths, and oversized metadata. The control-grant and reader-feature documentation fixtures passed. The documentation checker found 55 English and 55 Chinese pages with zero tree, heading, relative-link, fragment, or JSON-example issues. Full outputs are under `logs/test/2026-11-05-generic-node/`. A temporary empty `XDG_CONFIG_HOME` keeps tests independent of an operator's active IsOS configuration. The clean-home owner replay passed for two simulator scenes in `logs/test/20261105-generic-node-bridge/replay-r29.log`: generic package install, graph Validate and Apply, Run readiness, IsOS key press/refresh/release through EOS, 20 camera frames per scene, a completed primitive command, and confirmed Stop with reaped simulator. The EOS and package-side changes used for that replay remain local and uncommitted; remaining negative and browser cases are tracked in [TODO](todo.md).

The 2026-11-06 configuration run used `cargo test --all-targets --no-fail-fast`, `cargo clippy --all-targets -- -D warnings`, `cargo fmt --all -- --check`, `python3 -m unittest discover -s tests -p 'test_install_user_service.py'`, `python3 tools/check_isos_docs.py`, and `git diff --check`. All passed; the documentation checker found 56 pages per language and zero issues. It also ran the installed `isos --help` and `isos version check --json` against the selected local home. Complete Cargo and installer output is in `logs/test/2026-11-06-config/`.

The source priority check runs `python3 -m unittest discover -s tests -p 'test_source_launcher_env.py'`. It verifies that `.env` wins over `.env.example` and that the example fills a missing value. The config unit test checks that selected TOML wins while the example fills missing fields.

[中文](../../../docs_cn/isos/maintenance/verification_cn.md)

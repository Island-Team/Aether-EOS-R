# P06 EOS event design fixtures

[中文](README_cn.md) · [owner contract](../../../docs/eos/interfaces/events.md) · [runtime design](../../../docs/eos/design/runtime/events.md)

`examples.jsonl` contains the three exact bilingual contract examples. `check_events.py` checks their UTF-8 JSONL envelope, field bounds, package digests, shared documentation links, English/Chinese equality and thirteen malformed variations. `cases.json` freezes 30 implementation and boundary replay cases by stable ID, mutation and expected result. The Rust producer is exercised by `cargo test -p aether-common`, `cargo test -p aether-runtime --test events`, and `python3 tests/jsonl_events_process.py`.

The EOS tests use fake monotonic time for the five-second suppression window, capture stderr from all three persistent binaries, preserve runtime stdout ready, and launch a stub MuJoCo child to check startup, backend loss and stop-before-reap. Pair the IsOS P06 capture replay with the same `run_id`, and verify that a package/Apply failure before spawn has only the IsOS manager-session ID. Inspect parsed fields as well as raw bytes for secret, prompt, frame, argument, path and child-output leaks. `eos_ready` is a local runtime transition; IsOS status is the deployment-readiness authority.

Run `python3 tests/fixtures/jsonl_events/check_events.py` from the repository root. Keep full implementation test output in ignored `logs/test/<run-id>/`.

`browser_result_cases.json` freezes the Q01 EOS bridge producer boundaries, including the deterministic 128-byte UTF-8 token, response mismatch, repeat suppression, bounded cache and sink failure. The bridge's Rust unit and private-runtime stub tests exercise these cases; IsOS selected-child admission and P07 replay have separate owner evidence.

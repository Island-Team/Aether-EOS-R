# Q04 replay storage

[中文](../../docs_cn/island/q04-home-replay_cn.md) · [Q01 qualification](q01-first-use-design.md)

The selected Q04 replay uses `tempfile.gettempdir()` for the M4 clean home, camera case home, and R06/A03 fault homes. Set `TMPDIR` to a short, writable path on the larger filesystem before starting the replay, for example `/home/operator/q04t`. Keep the selected source checkouts, package bundle, build outputs, browser capture, and `logs/test/<run-id>/` archive under `/home`. A short temp root is necessary because the runtime and simulator use Unix domain sockets with limited path length.

The selection must be frozen **after** this harness change and the MuJoCo release input have merged to their integration branches. Its harness digest records the exact replay scripts and `q01_selection.py`. Verify that every selected executable and UI build is a real file beneath the selected source checkout, with no symlink in its path ancestors. Retain all failed-run artifacts; start a new run ID for any retry.

Focused check on Linux x86-64, 2026-11-03: `TMPDIR=/home/operator/q04t PYTHONPATH=tests python3 -m unittest tests/test_q01_selection.py -v` passed 13 tests. The new test launches a fresh Python process with `TMPDIR` under `/home` and binds a Unix socket in the resulting replay home; it also rejects path escapes. Complete output: ignored `logs/test/20261103-q04-home-harness/selection-unit.log`.

Before review, the focused selection, harness guard, and archive path suites passed 23 tests. `cargo fmt --all -- --check`, workspace Clippy with warnings denied, serial workspace tests (52 passing test groups), `cargo build --workspace`, Python byte compilation, and `python3 scripts/check_doc_links.py --all` passed. Reproduce with `TMPDIR=/home/operator/q04t`; complete outputs are in ignored `logs/test/20261103-q04-home-harness/`.

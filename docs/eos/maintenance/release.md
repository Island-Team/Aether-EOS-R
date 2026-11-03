# Public release checks

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Maintenance](README.md) / **release**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [maintenance/](README.md)
      - [publish.md](publish.md)
      - **release.md**

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/maintenance/release_cn.md)

Deployment configuration, native product skills, workflow templates, site-specific scripts, and runtime state are kept locally and ignored by Git. A clean checkout builds EOS core; simulation runs through the external MuJoCo package managed by IsOS.

Before publishing, review tracked source, tests, documentation, and Git history for site names, endpoints, device identities, calibration, saved routes, credentials, captures, and private dependencies. Validate the build and tests from a clean checkout, then check documentation links and the release diff.

Removing a file in a new commit does not erase it from earlier commits or tags. Any release that requires complete historical removal needs a separately planned history rewrite and replacement of published refs.

## 0.2.0 candidate review and first use

The `0.2.0` public source snapshot is a **candidate** until the independent public PR, clean checkout gates, and cross-repository first-use qualification pass. Review the [tracked-file receipt](publish.md#prepare-the-020-source-candidate) path by path. Search the tracked tree and complete public diff for credentials, keys, private hostnames or LAN endpoints, robot and device identities, calibration, routes, captures, prompts, site-specific scripts, local home paths in runnable defaults, and private dependencies. Classify historical local paths in evidence reports separately: they identify past test environments and must not be silently rewritten as new test results. A private opt-in skill directory is an explicit public snapshot exclusion; check that no public source or documentation path requires it, while preserving its private callable ABI. Verify that ignored deployment files, generated configuration, installed packages, runtime logs, and test archives are absent from the public tracked-file receipt. Review public Git history separately; deleting a tracked file in a new commit does not remove an older public copy.

On the release candidate, set `TMPDIR` and `CARGO_TARGET_DIR` to roomy directories outside the checkout when the root filesystem is small. Run `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace --all-targets -- --test-threads=1`, `cargo build --locked --workspace --bins`, `cmake -S . -B "$BUILD_DIR" -DISLAND_BUILD_TESTS=ON`, `cmake --build "$BUILD_DIR"`, `ctest --test-dir "$BUILD_DIR" --output-on-failure`, and `python3 scripts/check_doc_links.py --all` from a clean public checkout. Keep complete output and tool versions in the release receipt; compare the public result with the selected private gates before making a release claim.

For a first-use replay, follow the [EOS operator guide](../usage/README.md) with a clean IsOS home and the reviewed `island/mujoco-sim@0.1.1` asset. Build EOS from the candidate checkout, install and verify the package through IsOS, choose an available scene and device, then run `isos config validate <graph-file>` and `isos config apply <graph-file>`. Launch with `isos serve` and `isos run` in separate terminals; use `isos status --json` and `isos logs all --lines 200` to find readiness or the first failure. Inspect a camera or world resource, issue one admitted primitive with a correlated result, inject a failure and restart, then stop with `isos deployment stop` and confirm child reap. Test explicit legacy graph migration separately. Record selected source, graph revision, package and binary hashes, command output, and failure/stop evidence. Environment paths in this recipe are operator supplied; no checkout path or MuJoCo library path is assumed.

The optional selected P01/P03/U01 replay script takes `--mujoco-library-root <installed-mujoco-library-dir>` in addition to its owner, asset, release, and output paths. The recorded Q04 final-5 command in the historical report belongs to its pinned older harness and remains unchanged.

Private snapshot preparation on 2026-11-04 passed `cargo fmt --all -- --check`, workspace Clippy with `--all-targets -- -D warnings`, serial workspace tests on rerun, locked workspace binary build, the focused public-manifest and socket-path Python tests, Python compilation, a CMake/CTest core check, optional C++ configuration and private skill target build, and the Markdown link check. Complete transcripts are in ignored `logs/test/20261104-q04-eos-public-snapshot-prep/`. The first serial Cargo run had two timing-sensitive Dispatcher stop tests time out under concurrent load; both passed in isolated reruns and the complete serial workspace rerun passed. These are private preparation gates; the clean public snapshot gates remain required.

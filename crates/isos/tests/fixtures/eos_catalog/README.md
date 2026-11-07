# Release-test EOS catalog fixture

[中文](README_cn.md)

The `catalog.json` bytes are the `aether-profile catalog` output built from EOS `master` commit `25c2d64c420d7ae2402bd386ebb1cdb1b31f1a1c` at workspace version `0.2.0`. Their SHA-256 is `eda80c78e9b414249a8aca89756ae30b142ad03f3f61e7a792f9d438c0bd78d4`. Regenerate from that exact source with `cargo build --locked -p aether-runtime --bin aether-profile` and `aether-profile catalog`; compare the digest before changing the fixture.

`aether-profile` here is a narrow executable test stub for IsOS's saved graph, selected catalog and P01 revalidation tests. It accepts `catalog` and a single `--node-types` sidecar with empty contribution arrays. It rejects other commands and nonempty contributions. The release workflow selects it only for `cargo test` through the `#[cfg(test)]`-only `ISOS_TEST_CATALOG_PROFILE` hook. The hook validates an absolute executable path and fails if it is absent. Production `core_bins::resolve("aether-profile")` and runtime admission still require the installed EOS owner. Real EOS catalog, sidecar and readiness integration are qualified separately in Q04's selected-owner replay.

The initial CI prepare runs used no selected EOS owner, while developer machines had a sibling EOS binary. The test fixture makes the unit suite independent of that sibling without claiming that a package-contributed adapter is compiled. Keep the missing-owner read-only and 503 assertions; do not replace an owner failure with this fixture in a production binary.

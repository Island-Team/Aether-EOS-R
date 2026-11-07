# Develop IsOS

This guide is for contributors changing the current Rust server. The single Cargo package builds `isos`; `main` enters `app`, which delegates graph and package decisions to `domain` and live state to `runtime`. Public node, package, core, and UI contracts live in [interfaces](../interfaces/README.md).

- [HTTP routes](http/README.md) lists local request domains, authentication, envelopes, and examples. It is for server and client implementers, while the external node-event boundary has its own [UI contract](../interfaces/ui/node-events.md).
- [Owner internals](owner/README.md) documents local manager IPC, service configuration, and log cursors. Core handoff and stop fields remain in the [external core contract](../interfaces/cores/README.md).
- [Server design](../design/server.md) explains module ownership and request flow so new handlers keep verification and runtime state in their owners.
- [Verification](../maintenance/verification.md) records reproducible commands and evidence for changed contracts and routes.

For a change, update the owning contract and Chinese mirror, test success and failure, then run `cargo fmt --all -- --check`, `cargo clippy --all-targets -- -D warnings`, and `cargo test --all-targets --no-fail-fast`. Save full output under ignored `logs/test/<run-id>/`. Keep package IDs out of code paths and examples.

[中文](../../../docs_cn/isos/develop/README_cn.md)

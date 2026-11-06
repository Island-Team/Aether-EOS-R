# Generic package contributions

This folder is for package publishers and core authors. IsOS currently treats every package by the same exact-pin and verified-byte path; `kind` is descriptive metadata only. A package contributes registered node types, resources, processes, data types, and optional node sets through `node_contributions`. The owning core validates any payload-specific semantics before readiness.

- [Release](release.md) defines the package identity, version, provenance, assets, and contribution envelope; it is the entry point for a publisher.
- [Resources](resources.md) defines stable IDs, kinds, display metadata, contained files, and digests that instances can select without path guessing.
- [Process](process.md) defines entrypoint, lifecycle adapter, host inputs, and resource references for an owner-run process.
- [Verification](verification.md) explains exact-pin, file, catalog, and owner capability checks applied before graph publication or launch.

The [registered node contract](../nodes/README.md) defines the same type and port rules for package and built-in nodes. The [core handoff](../cores/handoff.md) binds a verified process to one run and revision.

[中文](../../../../docs_cn/isos/interfaces/packages/README_cn.md)

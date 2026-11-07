# Core owner contracts

These pages are for cores that accept IsOS-admitted nodes. A core owns its execution, lifecycle adapter, health, and physical safety; IsOS verifies exact package bytes, registered placement, and a run fence before handoff. The exchanges below carry bounded identities and outcomes, not package-specific scene or device semantics.

- [Setup](setup.md) tells a core which verified graph and configuration it must read before becoming ready.
- [Handoff](handoff.md) binds a declared process, executable, resources, owner, run ID, and revision to one owner acceptance.
- [Events](events.md) reports owner state back to IsOS with enough identity to reject stale observations.
- [Control grants](control-grants.md) carries owner-scoped permission to issue control; a graph edge alone cannot grant actuation.
- [Dependent stop](dependent-stop.md) requires terminal evidence before IsOS treats stop as complete or permits replacement.

Private manager IPC and service configuration live in [development](../../develop/owner/README.md).

[中文](../../../../docs_cn/isos/interfaces/cores/README_cn.md)

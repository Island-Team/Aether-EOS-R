# Owner implementation notes

This folder is for implementers of the IsOS local manager and its configured services. These are internal control surfaces; package and core publishers should use the [external core contracts](../../interfaces/cores/README.md) for handoff and stop.

- [Manager](manager.md) defines the local operation IPC and revision-checked mutation envelope.
- [Services](services.md) defines the manager TOML entries that select local owners and launch children.
- [Log cursors](log-cursors.md) defines run-scoped log pagination and ordering for diagnostics.

[中文](../../../../docs_cn/isos/develop/owner/README_cn.md)

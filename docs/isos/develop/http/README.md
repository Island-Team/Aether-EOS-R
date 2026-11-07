# Local HTTP routes

This folder is for IsOS server and local-client implementers. The routes begin at `/api/isos/v1/`; a request passes local Host/Origin admission and is decoded by `app`, while graph, package, and runtime owners make the decision. The [external node-event request](../../interfaces/ui/node-events.md) is documented with the node contract.

- [Request admission](request-admission.md) explains loopback Host/Origin and CORS checks before a mutation is accepted.
- [Envelope](envelope.md) defines success and error response fields, including diagnostic code, stage, and graph path.
- [Version](version.md) returns API identity so a client can select its supported interface.
- [Graph](graph.md) covers draft validation, saved revision, and Apply; the saved instance schema is in [nodes](../../interfaces/nodes/instance.md).
- [Catalog](catalog.md) exposes registered types and selector options without changing the saved graph.
- [Packages](packages.md) covers install, link, inspect, and removal around exact verified pins.
- [Cores](cores.md) exposes owner discovery and readiness to local clients.
- [Runtime](runtime.md) covers Run, Restart, Stop, status, and logs with run/revision fences.
- [Presentations](presentations.md) exposes selected output metadata; [UI selection](../../interfaces/ui/selection.md) owns the generic display rules.

[中文](../../../../docs_cn/isos/develop/http/README_cn.md)

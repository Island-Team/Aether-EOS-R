# IsOS guide

IsOS (Island OS) is the product and the only graph core for new graph `0.2.0`. Its source lives in `crates/isos/` in this repository at the unreleased `0.2.1` target. It stores the whole deployment graph, verifies exact package pins and declared resources, serves the local API, and supervises EOS and EI as direct children. IsosUI source lives in `ui/`; MuJoCo remains an external verified package. Supported older graphs are read-only until explicit migration.  A **registered type** defines a node's owning core, placement, settings, capabilities, ports, and lifecycle. An **instance** chooses that type and connects declared ports. The **owner** is the core that runs the node and validates its own payload semantics. A **run fence** ties events and handoffs to one active run and saved graph revision.

For an operator, follow this path:

- [First use](usage/README.md) starts a local home and shows the order for inspecting state, selecting verified nodes, validating, applying, and running.
- [Configuration](usage/configuration.md) explains which file supplies defaults, how production overrides are selected, and where to fix startup errors.
- [Package selection](usage/packages.md) explains how to install or link an exact release, inspect contributions, and choose stable resource IDs without depending on package-specific paths.
- [Publisher sign-in](usage/authentication.md) explains the separate Island-port CLI login used to publish packages; local graph control does not use this credential.
- [Run a deployment](usage/run.md) covers Validate, Apply, Run, Restart, Stop, and state inspection using the saved revision and owner outcome.
- [Troubleshooting](debug/troubleshooting.md) locates package rejection, launch, readiness, log, and uncertain-stop failures using structured diagnostics.

For an author or contributor:

- [External interfaces](interfaces/README.md) contains the generic node, package, core, and IsosUI contracts that publishers and graph authors need.
- [IsOS type catalog](layers/README.md) describes the currently registered IsOS-owned process family and explains UI-owned consumers.
- [Design](design/README.md) explains module ownership, package admission, data selection, and the keyboard-to-base control path without redefining wire fields.
- [Development](develop/README.md) locates local HTTP routes, manager IPC, source boundaries, and verification commands for implementers.
- [Maintenance](maintenance/README.md) indexes current evidence and remaining integration gates.

The current IsOS API uses `/api/isos/v1/*`; JSON payloads that require a schema use `"schema_version":"0.1.0"`. Package and core authors start at [external interfaces](interfaces/README.md).

[中文](../../docs_cn/isos/README_cn.md)

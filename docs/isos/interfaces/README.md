# Interfaces for nodes, packages, cores, and IsosUI

This is the external contract map for authors connecting nodes to IsOS. A **registered type** declares placement, settings, ports, and lifecycle; an **instance** selects that type and supplies values. A **core** owns execution. An **exact pin** identifies one verified package release. A **run fence** combines run identity and saved graph revision so a delayed event cannot affect a replacement run. IsOS currently admits graph declarations and packages through these contracts; runtime delivery remains the selected owner's responsibility.

- [Nodes](nodes/README.md) defines the generic type, instance, port, data type, selector, and edge rules used by every node family. Start there when publishing a type or wiring a graph.
- [Packages](packages/README.md) defines exact releases, declared resources, processes, and verification. Publishers use it to contribute node types without package-specific IsOS logic.
- [Cores](cores/README.md) defines the owner exchanges for setup, process handoff, events, grants, and terminal stop. Core authors use it to accept verified work and report outcomes.
- [UI](ui/README.md) defines node input events and generic presentation selection. IsosUI uses these boundaries to send user signals and display owner outputs.

Private manager IPC, local HTTP route details, and source workflows are documented in [development](../develop/README.md). The [control path walkthrough](../design/control-path.md) shows one composition of these generic contracts.

[中文](../../../docs_cn/isos/interfaces/README_cn.md)

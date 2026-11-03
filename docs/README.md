# Documentation

[中文](../docs_cn/README_cn.md) · [Repository README](../README.md)

Read in first-use order: [Island architecture](island/architecture.md) → [layer connections and data flow](island/layer-dataflow.md) → [run EOS](eos/usage/README.md) → [EOS core, layers, groups and nodes](eos/layers/README.md) → [owned interfaces](eos/interfaces/README.md) → [runtime algorithms](eos/design/runtime/README.md) → [implementation milestones](island/implementation-milestones.md) → [detailed TODO](island/implementation-todo.md).

## Catalogs

| Scope | Home | Contents |
| --- | --- | --- |
| Shared Island | [Island](island/README.md) | Architecture, compatibility, package rules, layout, and [conventions](island/conventions.v1.3.0.md). |
| EOS core | [EOS](eos/README.md) | First-use guide, registered node kinds, contracts, design, development, and debugging. |
| Registered EOS nodes | [Layers](eos/layers/README.md) | Canonical `core → layer → group → node` pages. An instance ID remains in the IsOS graph. |
| Historical compatibility | [Legacy](eos/legacy/README.md) | Opt-in C++ runtime, old Workflow/ActionList, and published ABI/protobuf identities. |

Field definitions and examples belong to the owning component's `interfaces/` tree. The 0.x target has no Workflow engine or graph-authored skill sequence; legacy documents remain accessible through the historical catalog.

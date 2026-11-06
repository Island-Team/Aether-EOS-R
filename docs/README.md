# Documentation

[中文](../docs_cn/README_cn.md) · [Repository README](../README.md)

Start with the [single-core unification design](island/unification-v0.2.1.md), then [IsOS usage](isos/usage/README.md), [EI](ei/README.md), [EOS usage](eos/usage/README.md), and [IsosUI](isosui/README.md). The [0.2.1 release draft](island/release-v0.2.1-draft.md) records pending checks; the [provenance map](island/source-provenance-v0.2.1.md) records imported histories.

## Catalogs

| Scope | Home | Contents |
| --- | --- | --- |
| Shared Island | [Island](island/README.md) | Architecture, compatibility, package rules, layout, and [conventions](island/conventions.v1.3.1.md). |
| IsOS manager | [IsOS](isos/README.md) | Graph authority, migration, packages, and process supervision. |
| EI domain | [EI](ei/README.md) | Cognition, safety, Python interfaces, and EGIA compatibility. |
| EOS domain | [EOS](eos/README.md) | Execution, registered hardware, Dispatcher, contracts, and runtime debugging. |
| IsosUI | [Workbench](isosui/README.md) | Browser graph editing and owner state. |
| Registered EOS nodes | [Layers](eos/layers/README.md) | Canonical `core → layer → group → node` pages. An instance ID remains in the IsOS graph. |
| Historical compatibility | [Legacy](eos/legacy/README.md) | Opt-in C++ runtime, old Workflow/ActionList, and published ABI/protobuf identities. |

Field definitions and examples belong to the owning component's `interfaces/` tree. The 0.x target has no Workflow engine or graph-authored skill sequence; legacy documents remain accessible through the historical catalog.

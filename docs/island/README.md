# IsOS: Island OS

[中文](../../docs_cn/island/README_cn.md) · [Documentation catalog](../README.md) · [0.2.1 unification](unification-v0.2.1.md)

IsOS brings four source components into one repository and one deployment graph core: IsOS manages the graph and processes; EOS (Embodied OS) executes robot operations; EI (Embodied Intelligence, formerly EGIA) makes embodied decisions; IsosUI is the local editor and workbench. EOS and EI remain separate processes. The verified `island/mujoco-sim@0.1.1` simulator package remains external. The unified source target is **0.2.1, unreleased**; there is no `v0.2.1` product tag or release.

| Component | Source | Guide | Owner |
| --- | --- | --- | --- |
| IsOS | `crates/isos/` | [IsOS](../isos/README.md) | Saved graph, packages, revisions, process lifecycle, local API and logs |
| EOS | `crates/aether-*` | [EOS](../eos/README.md) | Hardware adapters, health, Dispatcher admission and simulator child |
| EI | `ei/` | [EI](../ei/README.md) | Cognition, stage data, model calls and EI safety |
| IsosUI | `ui/` | [IsosUI](../isosui/README.md) | Graph editing, owner status, logs, Vision and admitted control UI |

New graphs use format `0.2.0` with one `isos` core. Manager layers keep their names; EOS domains use `eos_*`, EI domains use `ei_*`, and browser nodes use `ui`. New registered type IDs use `isos.<layer>.<name>@v0.2`. The four source components use version `0.2.1`; wire and external package versions remain independent. See [architecture](architecture.md), [data flow](layer-dataflow.md), and [repository layout](layout.md).

Start with [IsOS first use](../isos/usage/README.md), then inspect the [interface catalog](interfaces/README.md). [Conventions](conventions.v1.3.1.md) govern source and documentation changes. The [implementation milestones](implementation-milestones.md), [0.2.1 release draft](release-v0.2.1-draft.md) and dated qualification records describe work and evidence; historical release claims remain tied to their original commits and versions.

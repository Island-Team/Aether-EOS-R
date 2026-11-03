# EOS documentation

[中文](../../docs_cn/eos/README_cn.md) · [Island architecture](../island/architecture.md) · [Layer connections and data flow](../island/layer-dataflow.md) · [Shared conventions](../island/conventions.v1.3.0.md)

EOS is the Rust robot execution core. Its registered types follow `core → layer → group → node`; the IsOS graph supplies configured instances. The default process and the documented 0.x target are distinguished on each focused page.

## Read in first-use order

1. [Run EOS](usage/README.md): build, select a verified MuJoCo package, validate and apply a graph, launch, inspect status, read logs, and stop.
2. [Layer connections and data flow](../island/layer-dataflow.md): follow typed edges, service bridges, admission, feedback and stop across cores.
3. [Node catalog](layers/README.md): find hardware, dispatcher, control, process, and interface node kinds by placement.
4. [Interfaces](interfaces/README.md): check EOS-owned node, payload, control, gRPC, and adapter contracts.
5. [Runtime design](design/runtime/README.md): understand lifecycle, admission, edge scheduling, and failure handling.
6. [Development guide](develop/README.md): change an implementation boundary and run focused checks.

The [historical catalog](legacy/README.md) identifies the opt-in C++ and Workflow/ActionList pages. The 0.x target does not include a workflow engine or graph-authored skill sequence. [Debugging](debug/README.md), [backend references](reference/backends/README.md), and [implementation milestones](maintenance/todo.md) cover the remaining tasks.

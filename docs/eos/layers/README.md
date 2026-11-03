# EOS layers and node types

[中文](../../../docs_cn/eos/layers/README_cn.md)

The EOS core is organized as `core → layer → group → node`. Each leaf documents a registered **node type**, while a deployment graph supplies configured instances. This catalog follows current code and marks planned `@v0.1` behavior separately. IsOS owns the graph contract; EOS owns these node and payload contracts.

- [control](control/README.md)
- [dispatcher](dispatcher/README.md)
- [hardware](hardware/README.md)
- [interface](interface/README.md)
- [process](process/README.md)

The old `eos.control.skill@v1.0` graph-authored sequence and C++ Workflow/ActionList docs are compatibility history; the 0.x target does not add a workflow engine or graph-authored skill sequence.

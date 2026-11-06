# Generic node contracts

This folder is for type publishers, graph authors, and UI developers. IsOS currently applies one declaration model to UI producers, bridges, primitive bindings, hardware, and package processes. Types describe allowed behavior; instances select values and connect declared ports.

- [Registered type](type.md) defines owner placement, lifecycle, capabilities, settings, ports, and bridge authority; registration validates these declarations once.
- [Instance](instance.md) defines the saved identity, exact package pin, selected settings, and input references; admission checks them against the type.
- [Ports](ports.md) defines data type, transport, delivery mode, cardinality, and rate declarations; an anchor represents this contract rather than a URL.
- [Data types](data-types.md) defines codec, media type, schema, and payload bound, shared by control signals and presentable outputs.
- [Settings and selectors](selectors.md) defines generic JSON value kinds and provider-backed choices, so editors need no node-specific form rule.
- [Edges](edges.md) defines typed references and explicit bridges between communication methods and owners.

[中文](../../../../docs_cn/isos/interfaces/nodes/README_cn.md)

# Graph edges

This page is for graph authors connecting registered node ports. IsOS currently admits a connection only when its source and destination exist in the saved graph, their registered placements and ports agree, and the declared data and transport are compatible. The graph stores references; it does not store endpoint URLs or access grants.

An input maps to `{"node":"core/layer/group/node","output":"port"}`; a declared many-input may hold an array of such references. `node` is a canonical four-component path and `output` names an output port of that node. The destination key names a registered input port. A missing node, undeclared port, duplicate input, mismatched data type, transport, or delivery mode, or a cycle fails admission with a graph-path diagnostic. Cross-core transport requires an explicit declared bridge capability. [Ports](ports.md) are the source of truth for connection limits and communication method.

For the first keyboard control path, the producer's key-state output connects to an IsOS HTTP gateway, then an EOS HTTP gateway, then a typed primitive binding and hardware. Each connection is visible in the graph; [the design walkthrough](../../design/control-path.md) explains its runtime behavior. A bridge translates communication method at its own boundary; an arbitrary cross-core edge cannot silently become HTTP.

[中文](../../../../docs_cn/isos/interfaces/nodes/edges_cn.md)

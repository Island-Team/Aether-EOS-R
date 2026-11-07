# Output selection

This design explains why IsOS selects presentation metadata without becoming a producer. The [data interfaces](../interfaces/ui/README.md) define output types and selection; the [HTTP presentation route](../develop/http/presentations.md) defines the current response.

A registered output port and its `DataType` identify what a client could display. A descriptor from the owning producer would also bind representation, delivery method, endpoint identity, run ID, and graph revision. The producer owns payload bytes and endpoint authorization. IsOS does not infer image, depth, detection, or audio semantics from a package ID or type name.

The current route rejects malformed or stale requests and returns unavailable while no verified public owner descriptor is registered. It does not return a fabricated endpoint. Owner descriptor integration is an [outstanding gate](../maintenance/todo.md).

[中文](../../../docs_cn/isos/design/data_cn.md)

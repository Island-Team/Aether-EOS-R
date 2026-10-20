# External process interfaces

[Interface guide](../README.md) · [中文](../../../../../docs_cn/eos/develop/interfaces/external/README_cn.md)

These interfaces connect EOS or its adapter to other processes. Their transport does not define a compass-direction namespace.

- [HTTP/SSE adapter](http.md): WebUI entry point; the adapter calls EOS gRPC services.
- [Model providers](models.md): EOS outbound capability calls over HTTP, WebSocket, or configured gRPC; private schemas live in `share/contracts/eos/external/models/schemas/`.
- [Standalone simulator](simulator.md): private EOS gRPC over a Unix socket and its own WebUI HTTP/WebSocket API.

The [skill mailbox](../skills/native/ipc.md) is documented with native skill authoring, because it belongs to the skill runtime.

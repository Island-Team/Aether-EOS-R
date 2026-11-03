# External process interfaces

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Interfaces](../README.md) / [External](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [interfaces/](../README.md)
      - [external/](README.md)
        - [http.md](http.md)
        - [models.md](models.md)
        - [simulator.md](simulator.md)

</details>
<!-- docs-tree:end -->

[Interface guide](../README.md) · [中文](../../../../docs_cn/eos/interfaces/external/README_cn.md)

These interfaces belong to the opt-in C++ side chain and connect its EOS process or adapter to other processes. The default Rust v2 HTTP and MuJoCo contracts are documented in [service processes](../../develop/services.md) and [MuJoCo v2](../mujoco.md). Transport does not define a compass-direction namespace.

- [HTTP/SSE adapter](http.md): WebUI entry point; the adapter calls EOS gRPC services.
- [Model providers](models.md): EOS outbound capability calls over HTTP, WebSocket, or configured gRPC; private schemas live in `share/eos/contracts/eos/external/models/schemas/`.
- [External simulator](simulator.md): the historical private Unix socket contract for the C++ MuJoCo client.

The [skill mailbox](../skills/native/community.md) is documented with native skill authoring, because it belongs to the skill runtime.

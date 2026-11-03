# 外部进程接口

[接口指南](../README_cn.md) · [English](../../../../docs/eos/interfaces/external/README.md)

这些接口属于可选 C++ 侧链，连接其中的 EOS 进程或适配器与其他进程。默认 Rust v2 的 HTTP 与 MuJoCo 契约见[服务进程](../../develop/services_cn.md)和 [MuJoCo v2](../mujoco_cn.md)。传输协议不构成方位命名空间。

- [HTTP/SSE 适配器](http_cn.md)：WebUI 入口；适配器调用 EOS gRPC 服务。
- [模型提供方](models_cn.md)：EOS 通过 HTTP、WebSocket 或配置的 gRPC 发起能力请求；私有 schema 位于 `share/eos/contracts/eos/external/models/schemas/`。
- [外部模拟器](simulator_cn.md)：C++ MuJoCo 客户端的历史私有 Unix socket 契约。

[技能 mailbox](../skills/native/ipc_cn.md) 属于技能运行时，归在原生技能编写文档中。

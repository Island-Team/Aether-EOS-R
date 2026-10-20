# 外部进程接口

[接口指南](../README_cn.md) · [English](../../../../../docs/eos/develop/interfaces/external/README.md)

这些接口连接 EOS 或其适配器与其他进程。传输协议不构成方位命名空间。

- [HTTP/SSE 适配器](http_cn.md)：WebUI 入口；适配器调用 EOS gRPC 服务。
- [模型提供方](models_cn.md)：EOS 通过 HTTP、WebSocket 或配置的 gRPC 发起能力请求；私有 schema 位于 `share/contracts/eos/external/models/schemas/`。
- [独立模拟器](simulator_cn.md)：EOS 经 Unix 套接字使用私有 gRPC，模拟器另有自己的 WebUI HTTP/WebSocket API。

[技能 mailbox](../skills/native/ipc_cn.md) 属于技能运行时，归在原生技能编写文档中。

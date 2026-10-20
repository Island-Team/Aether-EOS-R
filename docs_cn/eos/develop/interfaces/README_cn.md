# EOS 接口指南

[English](../../../../docs/eos/develop/interfaces/README.md)

其他进程通过 `island.eos.v1` gRPC 服务使用 EOS。WebUI 使用独立的 HTTP/SSE 适配器，由适配器调用相同的 gRPC 服务。只有 EOS 通过原生 C++ HAL 访问硬件。契约源码按领域组织，而不是按方向或传输协议组织。

## EOS 进程接口

- [gRPC 服务和访问模型](grpc_cn.md)：Gateway、Skill 和 Workflow 服务、监听配置、请求边界，以及读取、计算和写入行为。
- [动作](actions_cn.md)、[本体](body_cn.md)、[世界](world_cn.md)、[系统](system_cn.md)、[机器人](robot_cn.md)、[感知](perception_cn.md)、[工作流](workflow_cn.md)和[技能](skills_cn.md)：八个契约领域。
- [契约源码分布](contracts_cn.md)与[注解](annotations_cn.md)：手工维护的 protobuf 和生成的校验产物。

## 其他接口

- [HTTP/SSE 适配器](external/http_cn.md)、[模型提供方](external/models_cn.md)和[模拟器](external/simulator_cn.md)：`external/` 中按名称区分的进程接口。
- [硬件 HAL](southbound/README_cn.md)：C++ 驱动契约和硬件数据流。
- [原生技能编写](skills/native/README_cn.md)：内置 C++ 技能、仍受支持的 ABI v2，以及本地 [mailbox IPC 指南](skills/native/ipc_cn.md)。[外部技能执行](skills/external_cn.md)目前尚不支持。

gRPC 可通过回环 TCP 或 Unix 域套接字跨进程调用。IPC 描述进程间连接；领域文件夹描述载荷和操作。内部 `runtime/northbound` 源码名称只是实现细节。

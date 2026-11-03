# EOS 接口目录

[English](../../../docs/eos/interfaces/README.md) · [EOS](../README_cn.md) · [节点目录](../layers/README_cn.md)

本目录是 EOS 拥有的契约的权威位置。IsOS 拥有图持久化、包注册表和管理 API；EOS 拥有注册节点类型、类型化硬件载荷、控制、适配器边界和进程 API。节点页链接至此，不重复定义字段。

| 边界 | 契约 | 实现状态 |
| --- | --- | --- |
| 注册节点配置与载荷 | [硬件](hardware_cn.md)、[节点贡献](node-contributions_cn.md)、[原语绑定](primitive-bindings_cn.md)、[图控制](control_cn.md)、[Vision](vision_cn.md) | 当前 Rust 行为及明确标出的 0.x 扩展。 |
| 所选二进制文件的读取能力 | [读取器特性与身份](reader-features_cn.md)、[setup 适配器](setup-v1_cn.md) | G05 版本数组已实现；P05 精确类型与特性预检仍是目标设计。 |
| 仿真与适配器 | [MuJoCo 绑定](mujoco_cn.md)、[R06 受管理停止](simulator-stop_cn.md)、[硬件中的包适配器 ABI](hardware_cn.md#软件包适配器共享库-abi) | R05 严格套接字已实现；R06 物理停止为目标设计。 |
| 进程 API | [Rust 控制与计划中的 gRPC](grpc_cn.md)、[CapabilityService](capabilities_cn.md)、[契约源映射](contracts_cn.md) | gRPC protobuf 身份已发布；默认 Rust 网关仍是目标设计。 |
| Rust 诊断事件 | [JSONL 事件生产者](events_cn.md) | P06 目标设计；IsOS 归属汇总与游标。 |
| 外部服务 | [HTTP/SSE](external/http_cn.md)、[模型提供方](external/models_cn.md)、[仿真兼容接口](external/simulator_cn.md) | 主要属于可选 C++ 兼容路径。 |

可选 C++ southbound HAL、SkillService、WorkflowService 和早期图设置适配器由[历史接口目录](../legacy/README_cn.md)索引。保留已发布的线协议和 ABI 身份以兼容现有调用方；0.x 目标不新增 Workflow 引擎。

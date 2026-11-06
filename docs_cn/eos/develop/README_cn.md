# 开发 EOS

[English](../../../docs/eos/develop/README.md) · [EOS 文档目录](../README_cn.md)

默认 Rust 进程组见 [v2 构建与启动指南](build-and-run_cn.md)和[v2 硬件开发指南](hardware_cn.md)。其余 C++ 模块指南描述未启动的侧链。
[v2 ROS 2 集成说明](ros2_cn.md)规定未来驱动移植的目标行为。

完成[使用 EOS](../usage/README_cn.md)后，按以下顺序阅读：先找到负责功能的模块，再确认边界，随后查看[接口指南](../interfaces/README_cn.md)，最后进入[运行时设计](../design/runtime/README_cn.md)追踪行为与故障。默认 Rust 工作区位于 `crates/aether-*`。`adapters/eos-cxx/` 包含显式启用的 C++ 支线及仍在使用的原生技能 SDK。公开的 `share/eos/contracts/eos/` 和 `share/eos/schemas/eos/` 资源也供 Rust 与 EGIA 使用；旧 C++ 进程退役后仍须保留其身份。

| 默认 Rust 职责 | Crate |
| --- | --- |
| 共享工具与图准入 | `aether-common`、`aether-node-profile` |
| 类型化硬件与 MuJoCo 传输 | `aether-hal`、`aether-mujoco` |
| 命令准入与进程所有权 | `aether-dispatcher`、`aether-runtime` |
| 客户端传输及服务 | `aether-control`、`aether-http-bridge`、`aether-config-server`、`aether-tui` |

## 显式启用的 C++ 支线及历史模块

下列模块路径描述保留的 C++ 源码与历史设计，并不表示当前 Rust 路径会执行 Workflow、ActionList 或任意模型权重。旧 north HTTP 服务在其可选接口仍受支持时，继续保留独立的 REST、SSE 与 WebSocket 路由。

1. 从[运行时模块总览](../design/runtime/README_cn.md)找到负责模块和源码目录；修改前先阅读模块的职责与依赖。
2. 从独立的[接口目录](../interfaces/README_cn.md)选择 gRPC、HTTP、动作、技能或硬件接口；[契约目录](../interfaces/contracts_cn.md)说明 proto 定义与生成产物。
3. 进入该模块的设计文档，检查状态变化、失败路径和测试，再修改代码。[扩展指南](ext/README_cn.md)说明后端和独立进程集成，包括 MuJoCo 基准模拟器。

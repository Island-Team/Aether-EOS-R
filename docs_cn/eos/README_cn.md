# EOS 文档

[English](../../docs/eos/README.md) · [Island 架构](../island/architecture_cn.md) · [层连接与数据流](../island/layer-dataflow_cn.md) · [共用约定](../island/conventions.v1.3.0_cn.md)

EOS（Embodied OS）是统一 IsOS 源码中的 Rust 执行域。新 `0.2.0` 图把 EOS 节点放在唯一 `isos` 核心的 `eos_*` 层，类型 ID 使用 `isos.<layer>.<name>@v0.2`。EOS 仍为受监管进程，负责设备适配器、实时健康、Dispatcher 准入路径和直接启动的 MuJoCo 子进程。源码版本 `0.2.1` 尚未发布；已有 EOS `0.2.0` 资格证据属于历史记录。专题接口页区分目标行为与已发布通信契约。

## 按首次使用顺序阅读

1. [运行 EOS](usage/README_cn.md)：构建、选择经过验证的 MuJoCo 包、校验并应用图、启动、查看状态和日志、停止。
2. [层连接与数据流](../island/layer-dataflow_cn.md)：追踪跨核心类型化边、服务桥接、准入、反馈与停止。
3. [节点目录](layers/README_cn.md)：按位置查找硬件、调度器、控制、进程和接口节点类型。
4. [接口](interfaces/README_cn.md)：查看 EOS 拥有的节点、载荷、控制、gRPC 与适配器契约。
5. [运行时设计](design/runtime/README_cn.md)：了解生命周期、准入、边调度与故障处理。
6. [开发指南](develop/README_cn.md)：修改实现边界并运行针对性检查。

[历史目录](legacy/README_cn.md)标识可选 C++ 与 Workflow/ActionList 页面。0.x 目标不包含工作流引擎或图内技能序列。[调试](debug/README_cn.md)、[实现里程碑](maintenance/todo_cn.md)涵盖后续任务。

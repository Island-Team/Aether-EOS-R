# EOS 历史文档

[English](../../../docs/eos/legacy/README.md) · [EOS 目录](../README_cn.md) · [0.x 节点目录](../layers/README_cn.md)

这些页面描述可选的 C++ 旁路或已发布的兼容身份。为已安装的调用方和迁移工作保留访问，但它们不定义 0.x 目标架构。替换实现时仍须保留已发布的 protobuf 包、字段号、ABI 符号和 socket 身份。

| 历史领域 | 页面 | 0.x 处理方式 |
| --- | --- | --- |
| C++ 运行时组合 | [运行时概览](../design/runtime/README_cn.md)、[初始化](../design/runtime/init_cn.md)、[硬件 HAL](../design/runtime/hardware-legacy_cn.md)、[内核](../design/runtime/kernel_cn.md)、[调度](../design/runtime/dispatch_cn.md)、[状态存储](../design/runtime/store_cn.md) | 在最后调用方与等效测试完成前保留参考。 |
| Workflow 和 ActionList | [ActionList 设计](../design/runtime/actionlist_cn.md)、[WorkflowService 契约](../interfaces/workflow_cn.md)、[北向适配器](../design/runtime/northbound/workflow_cn.md)、[工作流模板](../../../docs/eos/templates/workflow.md) | 仅历史契约。0.x 目标不包含工作流引擎或图内技能序列。 |
| C++ 部署 | [本地 TOML 配置](../usage/configure_cn.md)、[配置参考](../usage/configuration-reference_cn.md)、[后端驱动](../usage/backends_cn.md)、[ARM64 指南](../usage/arm64_cn.md)、[C++ 开发](../develop/ext/README_cn.md) | 明确标为可选 C++ 指南。首次使用 0.x 从[运行 EOS](../usage/README_cn.md)开始。 |
| 已发布原生技能契约 | [SkillService](../interfaces/skills_cn.md)、[原生 ABI](../interfaces/skills/native/abi_cn.md)、[编写指南](../interfaces/skills/native/README_cn.md) | 兼容身份；更改归属前先看当前技能边界设计。 |

历史页路径保持不变，以保护现有链接。本目录是这些页面的统一活动索引；核心、层、组和节点目录只收录当前或目标架构。

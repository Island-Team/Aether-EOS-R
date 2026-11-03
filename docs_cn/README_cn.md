# 文档

[English](../docs/README.md) · [仓库 README](../README_cn.md)

按首次使用顺序阅读：[Island 架构](island/architecture_cn.md) → [层连接与数据流](island/layer-dataflow_cn.md) → [运行 EOS](eos/usage/README_cn.md) → [EOS 核心、层、组、节点](eos/layers/README_cn.md) → [所属接口](eos/interfaces/README_cn.md) → [运行时算法](eos/design/runtime/README_cn.md) → [实现里程碑](island/implementation-milestones_cn.md) → [详细 TODO](island/implementation-todo_cn.md)。

## 目录

| 范围 | 首页 | 内容 |
| --- | --- | --- |
| Island 共用部分 | [Island](island/README_cn.md) | 架构、兼容性、包规则、布局和[约定](island/conventions.v1.3.0_cn.md)。 |
| EOS 核心 | [EOS](eos/README_cn.md) | 首次使用、注册节点类型、契约、设计、开发与调试。 |
| EOS 注册节点 | [层目录](eos/layers/README_cn.md) | 规范的 `core → layer → group → node` 页面。实例 ID 仍存于 IsOS 图。 |
| 历史兼容 | [历史目录](eos/legacy/README_cn.md) | 可选 C++ 运行时、旧 Workflow/ActionList，以及已发布的 ABI/protobuf 身份。 |

字段定义与示例放在所属组件的 `interfaces/` 树中。0.x 目标没有 Workflow 引擎或图内技能序列；历史文档仍可通过历史目录访问。

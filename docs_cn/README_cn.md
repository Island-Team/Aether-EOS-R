# 文档

[English](../docs/README.md) · [仓库 README](../README_cn.md)

首先阅读[单核心统一设计](island/unification-v0.2.1_cn.md)，然后阅读 [IsOS 使用](isos/usage/README_cn.md)、[EI](ei/README_cn.md)、[EOS 使用](eos/usage/README_cn.md)和 [IsosUI](isosui/README_cn.md)。[0.2.1 发布草稿](island/release-v0.2.1-draft_cn.md)记录待完成检查；[源码来源图](../docs/island/source-provenance-v0.2.1.md)记录导入历史。

## 目录

| 范围 | 首页 | 内容 |
| --- | --- | --- |
| Island 共用部分 | [Island](island/README_cn.md) | 架构、兼容性、包规则、布局和[约定](island/conventions.v1.3.1_cn.md)。 |
| IsOS 管理器 | [IsOS](isos/README_cn.md) | 图所有权、迁移、软件包和进程监护。 |
| EI 领域 | [EI](ei/README_cn.md) | 认知、安全、Python 接口和 EGIA 兼容。 |
| EOS 领域 | [EOS](eos/README_cn.md) | 执行、注册硬件、Dispatcher、契约和运行时调试。 |
| IsosUI | [工作台](isosui/README_cn.md) | 浏览器图编辑和所属进程状态。 |
| EOS 注册节点 | [层目录](eos/layers/README_cn.md) | 规范的 `core → layer → group → node` 页面。实例 ID 仍存于 IsOS 图。 |
| 历史兼容 | [历史目录](eos/legacy/README_cn.md) | 可选 C++ 运行时、旧 Workflow/ActionList，以及已发布的 ABI/protobuf 身份。 |

字段定义与示例放在所属组件的 `interfaces/` 树中。0.x 目标没有 Workflow 引擎或图内技能序列；历史文档仍可通过历史目录访问。

# Island 与 Aether

[0.x 架构与首次使用路径](architecture_cn.md)汇总所有权、目标版本、生命周期、兼容性和实现顺序。

[English](../../docs/island/README.md) · [文档目录](../README_cn.md)

**Island Team** 开发 **IslandOS Aether**，将智能体连接到模拟环境和真实机器人。Aether 将认知决策与物理执行放在不同进程中。**EGI Agent（EGIA）**理解操作意图、制定计划并请求有类型的动作；**Embodied OS（EOS）**接纳机器人设备、发布只读观测、运行技能，并在适配器触达硬件前检查命令。两者通过共享契约通信。本仓库包含 EOS 以及构建它所需的产品级文件，不包含其他服务的实现。

## 核心项目

| 项目或工具 | 职责 | 入口 |
| --- | --- | --- |
| **IsOS** | 管理已应用的 EOS/EGIA 节点图及单独配置的模型类服务；负责其进程生命周期和工作台。 | [IsOS 仓库](https://github.com/Island-Team/isos) |
| **Island-web** 与 [www.os-island.com](https://www.os-island.com) | 公开网站和浏览器界面，包含 IsOS 管理器与 EGIA 操作台。 | [Island-web 仓库](https://github.com/Island-Team/Island-web) |
| **Island-port** | Web 和服务网关，提供 IsOS 使用的软件包目录 API。 | [Island-port 仓库](https://github.com/Island-Team/Island-port) |
| **Island-egia** | 智能体认知、规划、记忆与编排；通过共享契约与 EOS 通信。 | [Island-egia 仓库](https://github.com/Island-Team/Island-egia) |
| **Island-eos / Aether-EOS-R** | 当前开发仓库及独立维护的公开 EOS 发布仓库；EOS 负责机器人执行和命令准入。 | [EOS 文档](../eos/README_cn.md) · [公开源码](https://github.com/Island-Team/Aether-EOS-R) |
| **Island-models** | 独立运行的语音、视觉、语言等模型服务，按配置供 Agent 或 EOS 使用。 | [组织总览](https://github.com/Island-Team) |
| **Island-Base 与机器人项目** | 设备专属适配器、控制器和硬件集成，位于可复用 EOS 运行时之外。 | [组织总览](https://github.com/Island-Team) · [组织总览](https://github.com/Island-Team) |

这里介绍 Aether 的核心项目，并非 Island Team 全部仓库的清单。其他研究、模拟和机器人项目见[组织总览](https://github.com/Island-Team)。

## 本仓库提供的工具

默认 EOS 运行时由 `eosd`、`eos-north-http`、`eos-config` 和 `eos-cli` 脚本启动 Rust 程序；IsOS 应用 v2 图并管理外层进程生命周期。`island-pkg` 客户端和 `island-launch` runtime stack 属于可选 C++ 兼容路径。先阅读 [EOS 使用指南](../eos/usage/README_cn.md)；维护侧链时再查看[旧版软件包指南](../eos/usage/packages_cn.md)及[清单契约](interfaces/packages_cn.md)。

## 产品级参考

- [层连接与数据流](layer-dataflow_cn.md)：类型化边、服务桥接、安全路径、故障传播及停止顺序。
- [0.x 详细实现 TODO](implementation-todo_cn.md)：任务所有者、依赖及验收测试。
- [Q01 所选重新启动重放](q01-restart_cn.md)：有限模型故障与显式新运行恢复证据。
- [Q01 集成首次使用设计](q01-first-use-design_cn.md)：所有者流程、冻结夹具及完整验证证据计划。

- [仓库布局](layout_cn.md)：本仓库包含什么，以及源码、契约、构建产物和本地状态的位置。
- [代码与文档约定](conventions.v1.3.0_cn.md)：Island 仓库共享的规则。
- [Aether–EOS 契约归属](contracts_aether_cn.md)：Agent 与 EOS 的边界及契约源码。

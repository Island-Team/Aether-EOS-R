# EGIA TODO

[English](../../docs/ei/todo.md)

本文是 Island-egia 唯一的实现路线图。`[x]` 表示已验证的当前基础，`[ ]` 表示尚未实现；目标设计不能被当成已交付功能。EGIA `0.1.0` 的运行时继续使用 Python，EOS 使用 Rust，IsOS 负责图、包和进程编排。

## 当前验证基线（2026-09-10）

- [x] 契约对齐的 MVP 有 562 个 pytest 测试通过；Ruff、Pyright 和 import-linter 检查通过。
- [x] EOS 规范契约、Python 类型模型、安装后的 schema 镜像保持一致；`islandctl validate`、prompt preview 和安装布局检查通过。
- [x] 在选定 EGIA `2e7abae` 提交运行受跟踪的 Q03 源码与已安装 wheel CI（[push 作业](../island/retired-repository-references_cn.md#ref-075162f7d189)、[PR 作业](../island/retired-repository-references_cn.md#ref-ef872dabc63c)）。

## 已实现的基础

- [x] **契约与 EOS link：** 固定的 EOS 协议类型、已验证模型、每个 backend 的客户端、必需发现重试、健康与 primitive 发现，以及可选 native skill 的语义发现。
- [x] **Prompt 所有权：** 一个 Agent 自有的 `etc/egia/prompts.toml` 管理 planner、proposer、sentinel、profile writer 和有优先级的动态上下文。
- [x] **Perception 与 context：** 有界观测准入、hearing、body/world 数据源、分组硬件、对象和检测摘要、完整动态消息预算以及明确的省略计数。
- [x] **Mission 与 decision：** 按优先级处理 Mission、可编辑语义 Plan、仅 primitive 的执行步骤、恢复反馈、proposer、sentinel、流式模型 I/O、token 计数和调用策略。
- [x] **Safety 与 monitor：** 按 Mission 的 primitive 准入、持续审计、心跳和停滞监控、硬界限、紧急停止锁存和直接紧急出口。
- [x] **Memory：** working、episodic、reference 存储，关键词与结构化召回、文档检索、整合、审核和可选 sqlite-vec 召回。
- [x] **运行时和操作员界面：** 外部 EOS profile、多 backend 启动、终端和浏览器接口、primitive/skill 目录及操作员 primitive 调用。
- [x] **Coordination：** 配置开关控制的 EOS 设备竞价者、拍卖、任务板、执行队列、区域租约、重新发现和确定性基准。

## 近期模块工作

- [x] 通过并发的快速 controller 和慢速 reasoner 显示 plan/execute 阶段；执行失败强制回到 plan 阶段。
- [ ] 统一模型调用与步骤边界的停机结果；为已接受步骤增加因果来源验证和并发资源安全评估。
- [ ] 协调 zone lease 与 Mission 抢占，合并多 EOS 场景投影，按需保存竞价与分配先验。
- [ ] 通过注入接口移除 `context → memory` 反向依赖。
- [x] 增加 CI 与安装 wheel 验证（[已合并 PR #103](../island/retired-repository-references_cn.md#ref-bd5748ad94f4)）。

## EGIA 0.1.0 实现里程碑

Python 是目标运行时。[节点目录](layers/README.md)、[目标架构](design/architecture.md)、[唯一阶段接口](interfaces/pipeline.md)和[首次使用](develop/build-and-run.md)描述目标。只有验收证据齐备才能完成里程碑。EOS Rust gRPC gateway 和 IsOS 图／配置读取器是跨仓库依赖。0.x 目标没有 WorkflowService、ActionList 或 workflow DAG；任务与计划是智能体状态模型。

| 里程碑 | 依赖 | 产出与验收 |
| --- | --- | --- |
| **E0 — 契约与兼容样例** | 当前 Python 核心、IsOS 图 I0 | `egia-config setup` 保留旧 identity 字段，同时声明支持的图／类型集合。注册 `@v0.1` 阶段说明、端口和设置；已知 `1.0.0`/`@v1.0` 只按既有安全行为读取。样例覆盖未来版本、可选字段扩展和带路径诊断。 |
| **E1 — 完整托管配置** | E0、IsOS I1/I3 配置布局 | 从完整安装默认值与已校验图覆盖生成 prompts、模型绑定、模式和安全策略。秘密只保留环境引用。从安装 wheel 启动按修订选定的 `EGIA_ETC` 快照；生成文件不原地编辑。 |
| **E2 — 图阶段组合** | E1、IsOS I1 目录 | 在一个 Python 进程内实例化 link、perception、context、missions、decision、safety、monitor；报告规范节点健康及关联 ID。固定 safety→link 与 monitor 紧急路径拒绝自定义绕过边。各阶段 mock 测试覆盖空值、畸形、重复、溢出及停止。 |
| **E3 — EOS gateway 集成** | E2、EOS Rust gRPC gateway、MuJoCo 0.1.1 包 | 按公开 EOS 契约验证发现、能力和终态操作。不完整发现阻止就绪；使用 IsOS 管理的真实模拟器测试重连、过期／重复操作、取消和紧急停止。 |
| **E4 — 统一日志与恢复** | E2–E3、IsOS I3 事件字段 | 脱敏事件映射运行／关联／节点字段，保留本地 replay 与检查点。测试只记一次启动、因果状态变化／终结顺序、轮转、脱敏、收集器丢失、游标筛选及重启后的重新发现。 |
| **E5 — 首次使用集成门槛** | E0–E4、IsOS I4–I6 | 安装 wheel 与 CI 检查，加上 IsOS → EOS → MuJoCo → EGIA 的全新环境运行，覆盖故障、重启、安全停止。完整输出保存 `logs/test/<run-id>/`；`0.1.0` 发布前在首次使用指南记录命令和实测结果。 |

[Q03 所有者资格验证设计](design/q03-qualification.md)记录 E5 的所选 Q01/Q02 前提、安装边界、固定基准、指标和证据。五轮基线、通过的 [EGIA `4db8319c` 源码与安装 CI 作业](../island/retired-repository-references_cn.md#ref-26e039c8e5e4)及封存的 EOS final17 Q01 源码进程重放，现由 [EGIA PR #108](../island/retired-repository-references_cn.md#ref-cfc2907c41c0) 与 [EOS PR #347](https://github.com/Island-Team/Island-eos/pull/347) 的已安装 EGIA 托管进程收据补足。输入 SHA-256 为 `3688ba488a91192e5b16b00cba1dae18a8a80e6578e2edad9794616c93b2dbee`；正向与负向封印分别为 `07dad1b546c58f3683798d5fbd225165c8052c2753b6c4f6b15e2d875759367b` 和 `5e179082da501496e197171bf61a8403b3d0083f0e8295310b08f39583504cd1`。已安装进程的 E5 所有者证据边界通过。EOS Q03 精确提交源码对账和 Q04 合并后发布资格验证仍各有检查门；此状态不代表已发布 `0.1.0`。

## 后续工作
- [ ] 先定义权限、隔离和审计契约，再引入受控工具；先定义有界、可供模型读取的契约，再实现 raster map。
- [ ] 只有 EOS 增加有 schema 的 native-skill 参数与能力声明后，才考虑 skill 执行；现在 skill 只是规划指导。
- [ ] 评估感知、上下文、推理、安全、执行和反馈的端到端时延，并建设轨迹、回放、数据治理与可重复基准。
- [ ] 学习式感知、世界模型、策略、fleet learning 与 artifact promotion 属于后续研究；`egia.tensor` 与包管理的模型权重选择不在本次发布范围。

任何未来学习模块都不能绕开 `decision → safety → link → EOS` 或取得物理释放权；连续控制与物理安全由 EOS 负责。

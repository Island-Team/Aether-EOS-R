# IsOS：Island OS

[English](../../docs/island/README.md) · [文档目录](../README_cn.md) · [0.2.1 合并设计](unification-v0.2.1_cn.md)

IsOS 将四个源码组件合并到一个仓库与一个部署图核心：IsOS 管理图和进程；EOS（Embodied OS）执行机器人操作；EI（Embodied Intelligence，原 EGIA）负责具身决策；IsosUI 是本地图编辑器与工作台。EOS 与 EI 仍为独立进程。经过验证的 `island/mujoco-sim@0.1.1` 模拟器软件包保持外部依赖。统一源码目标版本为 **0.2.1，尚未发布**；没有 `v0.2.1` 产品标签或发布版本。

| 组件 | 源码 | 指南 | 职责 |
| --- | --- | --- | --- |
| IsOS | `crates/isos/` | [IsOS](../isos/README_cn.md) | 保存图、软件包、修订、进程生命周期、本地 API 与日志 |
| EOS | `crates/aether-*` | [EOS](../eos/README_cn.md) | 硬件适配器、健康、Dispatcher 准入和模拟器子进程 |
| EI | `ei/` | [EI](../ei/README_cn.md) | 认知、阶段数据、模型调用和 EI 安全 |
| IsosUI | `ui/` | [IsosUI](../isosui/README_cn.md) | 图编辑、所有者状态、日志、Vision 与已准入控制界面 |

新图采用 `0.2.0` 格式，只有一个 `isos` 核心。管理器层保留原名；EOS 域使用 `eos_*`，EI 域使用 `ei_*`，浏览器节点使用 `ui`。新注册类型 ID 使用 `isos.<layer>.<name>@v0.2`。四个源码组件版本为 `0.2.1`；通信与外部软件包独立维护版本。参见[架构](architecture_cn.md)、[数据流](layer-dataflow_cn.md)与[仓库布局](layout_cn.md)。

先阅读 [IsOS 首次使用](../isos/usage/README_cn.md)，再查阅[接口目录](interfaces/README_cn.md)。源码和文档变更遵循[共用约定](conventions.v1.3.1_cn.md)。[实现里程碑](implementation-milestones_cn.md)、[0.2.1 发布草案](release-v0.2.1-draft_cn.md)及注明日期的资格记录描述任务与证据；历史发布声明仍绑定其原提交和版本。

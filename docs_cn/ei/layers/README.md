# EGIA 节点目录

[English](../../../docs/ei/layers/README.md) · [阶段接口](../interfaces/pipeline.md) · [架构](../design/architecture.md)

首次使用后可按 `core → layer → group → node` 在目录中定位节点。叶页说明用途、端口、生命周期及故障行为；字段定义以唯一的[阶段接口](../interfaces/pipeline.md)为准。以下七种 `@v0.1` 类型已有 A01 EGIA 侧 reader 和 managed 所有者绑定。stub 的 link → perception → context → decision 证据路径通过有类型端口运行。IsOS 仍需精确的 reader/catalog 更新，Apply/Run 才能选择这些图实例。0.x 目标没有 WorkflowService、ActionList 或 workflow DAG 节点；任务和计划是当前智能体状态模型。已知历史设计 ID 为 `egia.common.source@v1.0`、`egia.common.context@v1.0`、`egia.common.planner@v1.0` 与 `egia.common.guard@v1.0`；它们不是新阶段的别名，读取它们不会得到执行路径。

| 核心 | 层 | 组 | 节点类型 | 类型 ID |
| --- | --- | --- | --- | --- |
| `egia` | `io` | `link` | [gateway](io/groups/link/nodes/gateway.md) | `egia.link.gateway@v0.1` |
| `egia` | `evidence` | `perception` | [admission](evidence/groups/perception/nodes/admission.md) | `egia.perception.admission@v0.1` |
| `egia` | `cognition` | `context` | [builder](cognition/groups/context/nodes/builder.md) | `egia.context.builder@v0.1` |
| `egia` | `cognition` | `missions` | [queue](cognition/groups/missions/nodes/queue.md) | `egia.missions.queue@v0.1` |
| `egia` | `cognition` | `decision` | [planner](cognition/groups/decision/nodes/planner.md) | `egia.decision.planner@v0.1` |
| `egia` | `governance` | `safety` | [gate](governance/groups/safety/nodes/gate.md) | `egia.safety.gate@v0.1` |
| `egia` | `governance` | `monitor` | [health](governance/groups/monitor/nodes/health.md) | `egia.monitor.health@v0.1` |

注册类型说明解释行为与效果；每个图实例还携带操作员说明。图引用、来源边调度、包固定版本和修订校验由 IsOS 管理。固定的 safety→link 和 monitor 紧急路径不可被自定义边替换。模块算法继续放在 [`core/`](../core/modules.md)，目录提供面向操作员的各类节点入口。

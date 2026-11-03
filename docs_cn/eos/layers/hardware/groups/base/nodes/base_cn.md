# 移动底盘适配器节点

[English](../../../../../../../docs/eos/layers/hardware/groups/base/nodes/base.md) · [EOS 层目录](../../../../README_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings_cn.md) · [mujoco](../../../../../interfaces/mujoco_cn.md)

**位置：**`eos/hardware/base/<instance-id>` · **类型族：**`eos.hardware.base`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**`eos.hardware.base@v1.0` 和 `eos.hardware.base@v0.1` 均已注册；新图使用 `@v0.1`。只读旧图时不得改写其内容。

## 用途与边界

一个底盘拥有观测和物理停止接口。输入边描述命令路径，但不能直接授权执行。

## 端口与设置

输入：`command: BaseMovement`（可有多个兼容来源）。输出：`observation: BaseObservation`、`outcome: CommandOutcome`、`health: Health`、`status: Status`。仅允许声明过的 `body_velocity` 和/或 `relative_pose` 模式。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

解析已验证适配器，检查命令来源和类型化限值，经 Dispatcher 准入后才交给唯一适配器。租约到期或停止时调用适配器停止接口；无法确认完成时保留 `OutcomeUnknown`，不自动重试。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

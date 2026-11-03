# 机械臂原语绑定节点

[English](../../../../../../../docs/eos/layers/dispatcher/groups/manipulator/nodes/primitive-manipulator.md) · [EOS 层目录](../../../../README_cn.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md)

**位置：**`eos/dispatcher/manipulator/<instance-id>` · **类型族：**`eos.dispatcher.primitive_manipulator`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**`eos.dispatcher.primitive_manipulator@v1.0` 和 `eos.dispatcher.primitive_manipulator@v0.1` 均已注册；新图使用 `@v0.1`。只读旧图时不得改写其内容。

## 用途与边界

节点命名一个带版本的机械臂操作，并通过显式边连接兼容机械臂；设备仍由硬件适配器拥有。

计划中的 IsosUI EOS 面板把这个**活动实例**显示为一项可选原语控制。面板显示类型与实例描述、唯一目标、声明的运动模式及 EOS 实时健康；调用经现有 `primitive_typed` bridge，并显示关联结果。图连线是绑定，不是可执行调用。

## 端口与设置

设置：`primitive_name`、`primitive_version`、`movement_type`。输出：`command: ManipulatorMovement`；无输入。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

验证绑定目录、边目标、关节顺序和类型化命令头，再请求 Dispatcher 准入。机械臂运动失败或结果不确定时，经拥有设备的适配器停止。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

# 旧原语控制节点

[English](../../../../../../../docs/eos/layers/control/groups/primitive/nodes/primitive.md) · [EOS 层目录](../../../../README_cn.md) · [control](../../../../../interfaces/control_cn.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings_cn.md)

**位置：**`eos/control/primitive/<instance-id>` · **类型族：**`eos.control.primitive`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**仅兼容导入的旧图。实现完成后，新图使用 `eos.dispatcher.primitive_base@v0.1` 或 `eos.dispatcher.primitive_manipulator@v0.1`。当前身份为 `eos.control.primitive@v1.0`。只读旧图时不得改写其内容。

## 用途与边界

旧节点在设置中选择目标和运动模式。新绑定节点通过类型化图边指定目标。

## 端口与设置

必填设置：`target` 和 `movement_type`。输入：调度器 `status` 和一项匹配观测。输出：`request: ControlRequest`、`outcome: CommandOutcome`。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

对于导入的旧图，解析配置的目标和模式，检查引用和输入类型，再经 Dispatcher 提交。迁移到绑定节点必须显式进行带修订号校验的图写入；只读不会改变保存的字节。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

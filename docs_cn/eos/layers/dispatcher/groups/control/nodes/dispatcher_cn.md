# 共享控制调度器节点

[English](../../../../../../../docs/eos/layers/dispatcher/groups/control/nodes/dispatcher.md) · [EOS 层目录](../../../../README_cn.md) · [control](../../../../../interfaces/control_cn.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings_cn.md)

**位置：**`eos/dispatcher/control/<instance-id>` · **类型族：**`eos.dispatcher.control`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**当前 `@v1.0` 身份保留用于兼容。新图使用 `eos.dispatcher.control@v0.1` 表示共享准入方；原语绑定不创建第二个调度器。只读旧图时不得改写其内容。

## 用途与边界

唯一的执行器准入方在后端执行前检查身份、健康、新鲜度、期限、限值和安全状态。图节点表示该准入方及其类型化路径，不创建第二个调度器。

## 端口与设置

输入：`request: ControlRequest`。输出：`base_command: BaseMovement`、`manipulator_command: ManipulatorMovement`、`status: Status`。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

规范化请求，拒绝无效或过期任务，保留命令身份，记录准入，只执行一次，然后观测或停止。路由输入上的优先级只影响交付顺序，不绕过这些检查。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

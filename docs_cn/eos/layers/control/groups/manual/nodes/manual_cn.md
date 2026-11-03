# 手动控制节点

[English](../../../../../../../docs/eos/layers/control/groups/manual/nodes/manual.md) · [EOS 层目录](../../../../README_cn.md) · [control](../../../../../interfaces/control_cn.md) · [hardware](../../../../../interfaces/hardware_cn.md)

**位置：**`eos/control/manual/<instance-id>` · **类型族：**`eos.control.manual`。注册类型是可复用定义；部署时选择实例 ID 和非空实例说明。

**状态：**保留旧图类型以安全读取；目标 UI 控件通过 EOS 准入请求执行。当前身份为 `eos.control.manual@v1.0`。只读旧图时不得改写其内容。

## 用途与边界

表示用户选择的底盘或机械臂目标，并输出请求结果。它不拥有执行器，也不创建第二条命令路径。

## 端口与设置

必填设置：`target`，规范硬件路径。输入：调度器 `status`，以及匹配的底盘或机械臂观测之一。输出：`request: ControlRequest`、`outcome: CommandOutcome`。 所链接的接口页负责精确字段类型、单位、默认值、校验、示例和兼容规则。新类型定义必须有非空 `NodeType.description`；旧 sidecar 使用已记录的兼容回退。

## 生命周期与算法

从已准入图解析目标，按类型和限值验证用户运动，再通过 Dispatcher 提交。`manual_stop` 停止配置中的目标；调用者不能替换为另一条路径。 节点状态、拒绝原因和关联 ID 写入共享部署日志。

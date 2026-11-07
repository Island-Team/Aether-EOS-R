# UI 节点事件

本契约供通过注册输出端口发出用户输入的 IsosUI 节点使用。IsOS 当前为首个按键状态生产者提供 `POST /api/isos/v1/nodes/events`。端点检查本地 UI 的 Host 与 Origin，检查活动运行及保存的修订，解析图中声明的路径，并从网关节点设置推导运动值。浏览器不能提供速度或归属方端点。

```json
{"schema_version":"0.1.0","source_node":"isosui/input/keyboard/drive","output":"state","data_type":"key_state","run_id":"run-1","active_revision":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","payload":{"state":"pressed","activation_id":"press-1","sequence":1}}
```

| 字段 | 类型／单位 | 含义与验证 | 缺失／默认值 |
| --- | --- | --- | --- |
| `schema_version` | 准确字符串／负载版本 | `0.1.0`。 | 必填。 |
| `source_node` | 规范字符串／四段节点路径 | 活动图中的注册 UI 生产者；最长 200 字节，四个非空路径段，仅允许 ASCII 字母、数字、`_`、`-` 或 `.`。 | 必填。 |
| `output` | 准确字符串／输出端口 | 此生产者使用 `state`；必须由注册类型声明。 | 必填。 |
| `data_type` | 准确字符串／注册类型 | 已声明输出使用 `key_state`。 | 必填。 |
| `run_id` | 字符串／活动运行身份 | 防止前一次运行的事件影响当前运行；1–128 字节。 | 必填。 |
| `active_revision` | 小写 64 位十六进制字符串／保存图 SHA-256 | 将事件约束到已准入图。 | 必填。 |
| `payload.state` | 枚举／按键状态 | `pressed`、`refresh` 或 `released`。 | 必填。 |
| `payload.activation_id` | 非空字符串／按键持续身份 | 将一次物理按住和续租分组；1–64 个 ASCII 字母、数字、`_` 或 `-`；同一次运行内不得复用（每个来源及运行最多记录 65,536 次已结束按住）。 | 必填。 |
| `payload.sequence` | 正整数／事件顺序 | 在一次激活中严格递增；过期或乱序事件不能续租。 | 必填。 |

成功事件返回 IsOS 封套，其中 `data.command_id` 是生成的归属方命令身份，`data.state` 为 `active` 或 `released`。归属方响应字段不会转发。

生产者选择的按键或组合键是节点设置，不是事件字段。网关的目标绑定路径及 `x_m_s`、`y_m_s`、`yaw_rad_s`（米每秒及弧度每秒）、`valid_for_ms`（50–500 毫秒）都是保存的设置。IsosUI 在按住期间发送续租；IsOS 将续租限制为最高 20 Hz，同时始终允许有效的释放。按键抬起、失去焦点或卸载时释放；续租中断也会使租约到期。归属方决定物理驱动和停止。参见[控制路径](../../design/control-path_cn.md)和[注册选择器](../nodes/selectors_cn.md)。

[English](../../../../docs/isos/interfaces/ui/node-events.md)

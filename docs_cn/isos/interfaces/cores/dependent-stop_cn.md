# 依赖停止契约

[English](../../../../docs/isos/interfaces/cores/dependent-stop.md) · [设计](../../design/server_cn.md)

当前 IsOS 从选中的所有者管道读取严格且以换行结束的终止停止收据。收据使用模式 `1`。IsOS 将收据绑定到选中 `run_id`、所有者 PID、活动修订、已准入的包进程节点及预期部分。所有者证明其后代和物理停止；IsOS 独立证明自己的直接进程组退出。证明缺失、格式错误、过期、延迟或矛盾时产生 `unknown`。

| 收据字段 | 类型 / 单位 | 用途与验证 | 缺省值 |
| --- | --- | --- | --- |
| `schema_version`, `type` | 整数 `1`；字符串 `eos_terminal_stop` / 身份 | 当前精确接受的所有者收据格式。 | 必填。 |
| `run_id`, `active_revision` | 字符串 / 运行与 SHA-256 | 必须匹配选中运行和图修订。 | 必填。 |
| `eos_pid`, `backend_node` | 整数；图路径 / 所有者与节点 | 必须匹配已准入所有者进程及后端节点。 | 必填。 |
| `backend_pid`, `backend_pgid` | 整数或 null / 进程 ID | 绑定已准入后代进程组。 | 字段必填；值可为 null。 |
| `generation_id`, `stop_epoch` | 字符串或 null / 所有者代次 | 所有者提供的停止关联。 | 字段必填；值可为 null。 |
| `admission_closed`, `backend_group_drained`, `backend_reaped`, `sockets_removed` | 布尔值 / 停止事实 | 报告 `stopped` 前检查所有必需事实。 | 必填。 |
| `parts` | 数组 / 选中的物理结果 | 必须精确覆盖预期已准入部分。 | 必填。 |
| `outcome`, `reason_code` | 字符串及可选字符串 / 终止结果 | `unknown` 保持保守。 | 结果必填。 |

物理停止仍不确定时，Restart 不能替换该运行。

# 迁移至 EOS 能力动作接口

[English](../../../docs/eos/interfaces/capability-migration.md) · [能力接口](capabilities_cn.md) · [HTTP 适配器](external/http_cn.md) · [工作流接口](workflow_cn.md)

EOS 0.2.0 退役五个公开的 `GatewayService` primitive 动作 RPC：`CallPrimitive`、`StartPrimitive`、`GetPrimitive`、`WatchPrimitive` 与 `CancelPrimitive`。HTTP 适配器同时退役 `POST /v1/eos/primitives:call`、`POST /v1/eos/primitives:start`、`GET /v1/eos/primitives/{command_id}` 与 `POST /v1/eos/motions:cancel`。其他 v1 Gateway、Skill 与 Workflow 操作仍可使用。部署 1.0.0 前应迁移客户端，退役路由不再返回动作结果。

| 旧调用 | EOS 0.2.0 调用 |
| --- | --- |
| 从 `Discover` 选择 primitive 绑定，然后用名称、版本、设备 ID 调用 `CallPrimitive` 或 `StartPrimitive` | 用 `ListCapabilities` 选择规范名称，再以 `capabilityName`、object `arguments` 与 `operationId` 调用 `SubmitOperation` |
| `GetPrimitive(command_id)` | `GetOperation(operation_id)` |
| `WatchPrimitive(command_id)` | `WatchOperation(operation_id)`；HTTP 为 `GET /v2/eos/operations/{id}:watch` SSE |
| `CancelPrimitive(command_id)` | `CancelOperation(operation_id)`；HTTP 为 `POST /v2/eos/operations/{id}:cancel` |

operation ID 同时是 Dispatcher 命令 ID 和幂等键。迁移未完成命令时保留原有 ID；结果不确定时不能换一个 ID 再提交相同物理意图。提交可能先返回 `PENDING`，应查询或观察至终态。终态 `Operation` 包含原有 `primitive.ack`，其中区分准入与物理结果。`OUTCOME_UNKNOWN` 不允许自动再次释放命令。观察流断开只关闭观察；确需停止运动时应调用明确的 stop 能力。

HTTP 客户端先调用 `GET /v2/eos/capabilities`，再提交所选能力：

```sh
curl -sS -H 'Content-Type: application/json' \
  -d '{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","operationId":"move-42","arguments":{"linear_x_m_s":0.1,"linear_y_m_s":0,"angular_z_rad_s":0,"valid_for_ms":250}}' \
  http://127.0.0.1:16201/v2/eos/operations:submit
```

规范名称应视为不透明身份。`ListCapabilities` 也返回逻辑 primitive、版本及设备 ID 元数据，供精确选择。可用性是实时提示，Dispatcher 仍可能拒绝之后的命令。若后续需要能力名称，客户端应自行保存；重启恢复的操作保留 ID 与结果，但无法重建该目录别名。

新 ActionList 使用 `WorkflowAction.capability` 的 `capabilityName`，不再为新计划使用 `WorkflowAction.primitive` 的名称、版本和设备 ID。历史 primitive 字段仍可读取和执行，以兼容已保存的定义与日志；EOS 不会自动改写持久工作流数据。skill 目标和 Workflow 服务版本不变。REPL 使用 `capabilities`、`call CAPABILITY`、`operation ID|last`、`watch-operation ID|last` 与 `cancel-operation ID|last`；旧式 primitive `call` 和 `cancel-motion` 命令已退役。

EOS 0.2.0 是产品版本。`base.velocity@1.0.0` 等 primitive schema 版本、技能包版本以及内部 worker IPC 协议版本独立，不随本次迁移升级。原生技能 ABI v2 继续受支持，包括增量 v2.1 callback；v1 技能 ABI 仍不受支持。

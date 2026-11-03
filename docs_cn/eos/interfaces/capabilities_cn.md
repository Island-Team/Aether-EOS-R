# CapabilityService v2

[English](../../../docs/eos/interfaces/capabilities.md) · [接口指南](README_cn.md) · [primitive 参数](actions_cn.md) · [内核设计](../design/runtime/kernel_cn.md) · [2.0 迁移](capability-migration_cn.md)

`island.eos.v2.CapabilityService` 是面向智能体的公开原生 gRPC 动作接口。它发现已准入的**硬件能力**，并在提交操作时隐藏进程生成的设备 ID。EOS 拥有目录和操作状态；[Dispatcher](../design/runtime/dispatch_cn.md)仍校验并释放每个 primitive。资源、模型和技能能力目前尚未进入该服务。源码契约是 `share/eos/contracts/eos/capabilities.proto`。

此服务与现有 `eosd` gRPC listener 共用本地可信客户端部署边界；已合并的 R02 listener 没有逐客户端认证或授权。EOS [R03 受管理准入契约](grpc_cn.md#r03-执行器受治理准入目标契约)已合并并通过验收：受管理执行器写入要求按运行固定的授权与保存图活动修订，操作查询/取消按角色范围校验。R05/R06 与限定范围 M3 完成条件随后通过验收；见[M3 记录](../../island/m3-qualification_cn.md)。独立的 north HTTP 适配器现已通过 REST 与 SSE 暴露相同的 `/v2/eos` 路由；生成的 gRPC stub 仍可使用。`island.eos.v1` Gateway、Skill 和 Workflow 服务继续提供非 primitive 动作接口。Gateway 的五个公开 v1 primitive 动作 RPC 已在 EOS 0.2.0 退役。

## 身份与结果

设备能力名称为 `hardware/{robot_id}/{device_class}/{part_id}/{primitive}@{version}`，必要时对各部分进行百分号转义。隐式设备 `motion.stop` 使用相同形式。整机 `robot.stop` 和 `safety.estop` 使用 `runtime/{robot_id}/{primitive}@1.0.0`。目录在启动时拒绝重复名称。descriptor 还提供逻辑 `primitive`、`version` 与 `deviceId` 元数据，用于精确匹配设备绑定，而不必解析规范名称。普通设备动作的 `available` 反映实时健康状态；即使设备不健康，安全停止条目仍保持可用。发现能力不保证后续提交一定通过 Dispatcher 准入。

调用方提供 1–128 字节的 `operation_id`。它也是 Dispatcher 的持久命令 ID 与幂等键，由 Dispatcher 使用。相同请求重放同一个 pending 或终态命令；冲突复用返回 `ALREADY_EXISTS`，绝不二次释放。`Operation` 特意不包含 `capability_name`：重启后 Dispatcher 可按 ID 恢复命令与确认结果，却不持久保存调用方选择的目录别名。如果之后需要该名称，客户端应自行保存。

`Operation.status` 为 `PENDING`、`SUCCEEDED`、`REJECTED`、`FAILED`、`CANCELLED`、`TIMED_OUT` 或 `OUTCOME_UNKNOWN`。pending 时 `accepted` 不存在，也没有 acknowledgement。终态设置 `accepted`，并在 `acknowledgement` 中包含规范 `primitive.ack`。顶层 `reason_code` 仅在确认结果的准入原因是字符串时复制它；准入后的失败可能没有顶层代码，具体结果保存在 acknowledgement 内。不能因 `OUTCOME_UNKNOWN` 自动重试物理动作。

## RPC

### ListCapabilities

`ListCapabilities({})` 返回所有已准入目录名称、当前可用性和逻辑 primitive、版本、设备 ID，包括安全停止条目。设备 ID 是 EOS 逻辑身份，不是后端端点。例如：

```json
{"capabilities":[{"name":"hardware/demo/base/default/base.velocity@1.0.0","available":true,"primitive":"base.velocity","version":"1.0.0","deviceId":"base-0"},{"name":"runtime/demo/robot.stop@1.0.0","available":true,"primitive":"robot.stop","version":"1.0.0"}]}
```

### SubmitOperation

`SubmitOperation` 要求非空 `capability_name`、显式存在的 object `arguments` 和 `operation_id`。参数仍是对应精确 primitive 需验证的调用方对象；能力名称解析目标和版本。例如：

```json
{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","arguments":{"linear_x_m_s":0.2,"linear_y_m_s":0.0,"angular_z_rad_s":0.0,"valid_for_ms":250},"operationId":"velocity-42"}
```

提交启动命令后可能立即返回 pending：

```json
{"operationId":"velocity-42","status":"OPERATION_STATUS_PENDING"}
```

终态响应包含未改写的调度确认结果：

```json
{"operationId":"velocity-42","status":"OPERATION_STATUS_SUCCEEDED","accepted":true,"acknowledgement":{"type":"primitive.ack","primitive":"base.velocity","primitive_version":"1.0.0","accepted":true,"reason":null,"reason_code":null,"command_id":"velocity-42","sequence":1,"result":{"status":"succeeded"}}}
```

未知能力返回 `NOT_FOUND`；字段缺失或 ID 无效返回 `INVALID_ARGUMENT`。安全、健康、schema 和后端结果遵循现有[primitive 契约](actions_cn.md)。已完成的语义拒绝仍是 OK RPC，携带 `REJECTED` 与 acknowledgement。

### GetOperation 与 WatchOperation

两者均接收 `{"operationId":"velocity-42"}`。`GetOperation` 返回当前保留的命令状态。`WatchOperation` 先发出该状态；如状态是 pending，再等待一次终态更新。它没有 cursor 或历史重放流。取消 gRPC watch 只停止观察，不取消已接受命令。未知 ID 返回 `NOT_FOUND`。重启后，恢复的 prepared 命令可返回 `OUTCOME_UNKNOWN`，不会再次释放。

### CancelOperation

`CancelOperation` 接收同一个 operation ID，要求 Dispatcher 取消后返回当前或终态 `Operation`。未知 ID 返回 `NOT_FOUND`；不可取消命令遵守 Dispatcher 的 precondition failure。取消只针对该命令，不会停止同设备后续命令。需要停止运动时应显式使用 stop 能力。

HTTP 客户端使用 `GET /v2/eos/capabilities`、`POST /v2/eos/operations:submit`、`GET /v2/eos/operations/{id}`、`GET /v2/eos/operations/{id}:watch`（SSE）以及 `POST /v2/eos/operations/{id}:cancel`。v1 primitive HTTP 路由已在 EOS 0.2.0 退役；参见 [HTTP 契约](external/http_cn.md)与[迁移指南](capability-migration_cn.md)。

**相关：**[gRPC 访问与传输](grpc_cn.md)、[northbound 运行时](../design/runtime/northbound_cn.md)、[ActionList](../design/runtime/actionlist_cn.md)。

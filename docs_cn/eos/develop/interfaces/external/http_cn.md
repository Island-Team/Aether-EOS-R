# north HTTP 与 WebUI 契约

[North HTTP implementation](../../ext/north_http/README_cn.md) · [原生 gRPC 接口](../README_cn.md) · [配置](../../ext/north_http/config_cn.md) · [English](../../../../../docs/eos/develop/interfaces/external/http.md)

`eos-north-http` 是唯一面向浏览器的 northbound 进程。它把 REST 与 Server-Sent Events（SSE）以及实时监视 WebSocket 转换为 `eosd` 提供的 Gateway、Skill、Workflow gRPC 服务；EOS core 自身只监听 gRPC。`eos-repl` 也只使用这条 HTTP/SSE 边界， 因此其集成测试同时是仓库外 WebUI 的可执行客户端样例。

该进程为 configured `upstream` 构造一个 shared gRPC `Client`。其一个 channel backing 一个 generated Gateway stub、一个 Skill stub 与一个 Workflow stub；所有 REST call、SSE stream 与 readiness probe 复用该 set。HTTP `Server` 借用 client，并拥有 request metadata、deadline、active-call cancellation、health check、routing 与 wire conversion。参见 [module map](../../ext/north_http/README_cn.md#module-map)。

## 目录

- [启动、健康与配置](#启动健康与配置)
- [REST 路由](#rest-路由)
- [SSE 流](#sse-流)
- [审计路由](#审计路由)
- [错误、CORS 与 request ID](#错误cors-与-request-id)
- [运维与迁移](#运维与迁移)

## 启动、健康与配置

分别启动 EOS 与 north HTTP：

```sh
./bin/eosd --config "$PWD/etc/eos/eos.example.toml"
./bin/eos-north-http --config "$PWD/etc/eos/north-http.toml"
```

默认 HTTP listener 为 `127.0.0.1:16005`，upstream EOS 为 `127.0.0.1:16003`。EOS 尚未运行时 north HTTP 仍可启动：`/health/live` 返回 `200`，`/health/ready` 与依赖 EOS 的请求返回 `503`；EOS 恢复后 gRPC channel 会自动重连。启用 ActionList audit 时，readiness 还要求外部投影健康；audit degradation 不改变进程 liveness 或 EOS core health。

`etc/eos/north-http.toml` 使用严格 key 校验：

| Key | 默认值 | 含义 |
| --- | --- | --- |
| `host`, `port` | `127.0.0.1`, `16005` | HTTP/SSE listener |
| `upstream` | `127.0.0.1:16003` | EOS gRPC 地址 |
| `allow_remote` | `false` | 非 loopback bind 的显式开关 |
| `allowed_origins` | Island WebUI 与本地 3000 端口 | 精确 CORS allowlist |
| `max_request_bytes` | `67108864` | 最大请求 body |
| `max_connections` | `64` | accepted connection 总容量 |
| `max_inflight_requests` | `32` | unary request 容量 |
| `max_sse_streams` | `16` | SSE stream 容量 |
| `max_monitor_streams` | `4` | monitor WebSocket 容量 |
| `connect_timeout_ms` | `2000` | readiness deadline |
| `request_timeout_ms` | `630000` | unary/stream gRPC deadline；最大 630000 ms |
| `sse_heartbeat_ms` | `15000` | SSE heartbeat 周期 |

四个 connection-capacity 值都必须为正，且各自上限为 256。unary、SSE 与 monitor capacity 的总和不得超过 `max_connections`；parser 以 overflow-safe 方式执行该检查。

严格 `[action_list_audit]` table 默认为 `enabled = false`、`rotation_size_bytes = 16777216`、`rotation_files = 8`、`reconnect_delay_ms = 1000`；三个数值必须为正，上限依次为 268,435,456 bytes、64 files 与 60,000 ms，且 overflow-safe size/count product 上限为 256 MiB。已提交的 reference config 显式启用 audit。存储位置固定从 `${state}/north-http/action-list-audit` 派生，不提供用户 path key。

strict parser 与 default 属于 [config module](../../ext/north_http/config_cn.md)；runtime enforcement 属于 [server module](../../ext/north_http/server_cn.md)。更改 `upstream` 会改变单个 process client target，而不会创建 per-route channel configuration。

未知 key、错误类型、非正数限制以及未同时设置 `allow_remote = true` 的 非 loopback 地址都会在打开 listener 前失败。`allow_remote` 不提供认证或 加密；生产部署应保持 loopback，或由可信 reverse proxy 终止 TLS、认证、 授权、限流和公网 CORS 策略。

可执行程序支持 `--config`、`--etc`、`--contracts`、`--host`、`--port`、 `--upstream` 与 `--log-level`；endpoint/log CLI 值覆盖配置文件。源码环境使用 `bin/eos-north-http`；安装后使用 `/usr/bin/eos-north-http`，native 程序位于 `/usr/libexec/island`，默认配置为 `/etc/island/eos/north-http.toml`。wrapper 不依赖当前工作目录。

## REST 路由

实时监视使用 `GET /v1/eos/monitor/ws`，并转发 `GatewayService.WatchMonitor`。客户端首先发送版本 1 的 `monitor.subscribe` JSON，指定 `camera_device_id`；`microphone_device_id`、 `include_detections` 与 `detection_source` 可选。ready、status、audio 和 error 使用 JSON 文本；摄像头 JPEG 使用 `EOSM` 二进制封包（4 字节 magic、版本、类型、2 字节保留位、4 字节 big-endian metadata 长度、JSON metadata、JPEG）。接口不发送原始麦克风 PCM， origin 必须在 `allowed_origins` 中，断开浏览器会取消上游 gRPC stream。

版本 1 可请求与源 JPEG 精确对齐的检测： `{"version":1,"type":"monitor.subscribe","camera_device_id":"cam-0","include_detections":true}`。 `detection_source` 为 `"object"` 时订阅选中摄像头的物体检测；默认或 `"human"` 使用已配置的人体检测摄像头。ready 返回 `detection_status`。kind 1 是原始 JPEG，kind 2 携带完整 `detection.snapshot` 及其精确源 JPEG。二进制封包版本为 1；所有 64 位 metadata 值使用十进制字符串。 订阅校验失败时，error envelope 也使用版本 1。

请求与响应使用 protobuf JSON 名称。POST body 是严格 ProtoJSON，错误 JSON 或未知字段返回 `400`。path 字段优先，body 中冲突的 skill reference 或 execution ID 会被拒绝。

| Method | Path | gRPC operation |
| --- | --- | --- |
| `GET` | `/v1/eos/discover` | `GatewayService.Discover` |
| `GET` | `/v1/eos/status` | `GatewayService.GetStatus` |
| `GET` | `/v1/eos/resources` | `GatewayService.ListResources` |
| `GET` | `/v1/eos/resource?uri=...` | `GatewayService.ReadResource` |
| `GET` | `/v1/eos/perception/roles` | `GatewayService.GetPerceptionRoles` |
| `POST` | `/v1/eos/primitives:call` | `GatewayService.CallPrimitive` |
| `POST` | `/v1/eos/motions:cancel` | `GatewayService.CancelPrimitive` |
| `POST` | `/v1/eos/world/camera:capture` | `GatewayService.CaptureFrames` |
| `POST` | `/v1/eos/perception/vad:infer` | `GatewayService.InferVad` |
| `POST` | `/v1/eos/perception/humans:detect` | `GatewayService.DetectHumans` |
| `POST` | `/v1/eos/perception/human-poses:estimate` | `GatewayService.EstimateHumanPose` |
| `POST` | `/v1/eos/perception/faces:detect` | `GatewayService.DetectFaces` |
| `GET` | `/v1/eos/skills` | `SkillService.ListSkills` |
| `GET` | `/v1/eos/skills/{name}/versions/{version}` | `SkillService.GetSkill` |
| `POST` | `/v1/eos/skills/{name}/versions/{version}/executions` | `SkillService.StartExecution` |
| `GET` | `/v1/eos/executions` | `SkillService.ListExecutions` |
| `GET` | `/v1/eos/executions/{id}` | `SkillService.GetExecution` |
| `GET` | `/v1/eos/executions/{id}/state` | `SkillService.GetExecutionState` |
| `POST` | `/v1/eos/executions/{id}:terminate` | `SkillService.TerminateExecution` |
| `POST` | `/v1/eos/executions:terminate` | `SkillService.TerminateExecutions` |
| `POST` | `/v1/eos/executions/{id}:signal` | `SkillService.SignalExecution` |
| `GET` | `/v1/eos/executions/{id}/journal:follow` | `SkillService.FollowExecutionJournal` |
| `GET` | `/v1/eos/executions:monitor` | `SkillService.MonitorExecutions` |
| `GET` | `/v1/eos/workflow-templates` | `WorkflowService.ListWorkflowTemplates` |
| `GET` | `/v1/eos/workflow-templates/{name}` | `WorkflowService.GetWorkflowTemplate` |
| `POST` | `/v1/eos/workflow-templates/{name}:run` | `WorkflowService.RunWorkflowTemplate` |
| `POST` | `/v1/eos/workflows` | `WorkflowService.ApplyActionList` |
| `GET` | `/v1/eos/workflows` | `WorkflowService.ListActionLists` |
| `GET` | `/v1/eos/workflows/{id}` | `WorkflowService.GetActionList` |
| `GET` | `/v1/eos/runtime:snapshot` | `WorkflowService.GetRuntimeSnapshot` |
| `GET` | `/v1/eos/audit/status` | 本地 external-audit health 与 retained cursor |
| `GET` | `/v1/eos/audit/action-lists?limit=N` | 本地 retained audit records |

这四条 perception route 是 typed unary one-shot inference call。每个 body 通过 `modelId` 选择一个精确模型，并在 `input.data` 中以 ProtoJSON base64 携带 media；可选 threshold、aligned depth media 与 tracked person 都是 typed field。EOS 会拒绝 capability 不匹配的模型，且不会退回其他已配置模型。Face detection 只公开 recognition；face registration 仍是 primitive。 role discovery 只返回已配置的 `vad`、`human`、`pose` 与 `face` binding。 ProtoJSON 会省略值为 false 的 `continuous`，客户端应将缺失值规范化为 `false`。

protobuf annotation 是版本化路由来源；运行 `./scripts/print_contract_interfaces.py` 可输出完整 message 与 route inventory。

### JSON wire 规则

字段名使用 lower camel case；`uint64` 使用十进制 JSON string；timestamp 使用 RFC 3339；duration 形如 `"30s"`；`bytes` 使用标准 padded base64；enum 使用完整 symbolic name。ProtoJSON 会省略默认 scalar 与空 repeated field；客户端须把缺失的 boolean、sequence 和 array 分别规范化为 `false`、`"0"` 与 `[]`。POST body 严格拒绝未知字段与错误类型，path 字段具有权威性。

### Gateway payload

Gateway route 公开 discovery、status、resource、primitive call/cancel 与 frame capture；payload 的 schema 和语义见 [northbound contract](../grpc_cn.md) 与 [primitives](../actions-payloads_cn.md)。 资源描述符都包含 `uri`。设备资源通常带 `class`；schema-owned retained resource 可以只带 `schema_uri` 与 `history`，不保证存在 `class`。

Discovery 中每个 primitive binding 都包含 `parameters`，它是该设备 `arguments` object 的 JSON Schema；WebUI 应用它生成输入控件，并继续把 `limits`、`references` 与 `absoluteFrame` 作为独立的兼容字段和运行范围提示。 例如 posture binding 可返回 `{"parameters":{"type":"object","properties":{"posture":{"type":"string","enum":["stand","walk","lie"]}},"required":["posture"],"additionalProperties":false}}`。 旧版 EOS 可能省略 `parameters`，此时客户端保留 raw JSON input 或本地 schema fallback。

### Skill 与 execution

Skill route 返回 descriptor 与 schema，执行 route 使用完整 lifecycle snapshot；终止是协作式请求，客户端必须继续观察直到 terminal。journal 包含 lifecycle、出站 SIGNAL 与 checkpoint/activity/suppression record；诊断日志仅供 operator 查看，不进入公共 journal。monitor 提供带 reset/upsert/remove delta 的进程式视图。claims、replay、signal 与 lifecycle 见 [SkillService](../skills/native/README_cn.md)。

### Workflow 与 runtime snapshot

ActionList 包含 string `revision`、symbolic list/item status、timestamp 与完整 item snapshot；skill item 使用 `executionId`，primitive item 使用 `commandId`。`GET /v1/eos/workflows` 返回 `{"actionLists":[...]}`，live revision 与 fact event 使用下述 Workflow SSE。

`GET /v1/eos/runtime:snapshot` 返回 `capturedAt`、`executions`、`facts`、`actionLists` 与 `actionListCursor`。ActionList、fact 与 cursor 是一个 atomic view；skill execution 保持独立 sequence。客户端先用 snapshot bootstrap，再从 cursor 之后 watch。完整契约见 [WorkflowService](../workflow_cn.md)。 fresh idle runtime 的零 sequence 可能被省略，例如 cursor 只有 `epoch`；客户端应将其视为 `"0"`。

```sh
curl -sS http://127.0.0.1:16005/v1/eos/discover
curl -sS -H 'Content-Type: application/json' \
  -H 'X-Request-ID: operator-smoke-1' \
  -d '{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"018f-example","arguments":{"linear_x_m_s":0.1,"linear_y_m_s":0,"angular_z_rad_s":0,"valid_for_ms":250}}' \
  http://127.0.0.1:16005/v1/eos/primitives:call
```

## SSE 流

| Path | Event | `id` | 恢复输入 |
| --- | --- | --- | --- |
| `/v1/eos/resource:watch?uri=...` | `resource-changed` | 无 | 无 |
| `/v1/eos/executions/{id}:watch?afterSequence=N` | `skill-execution` | execution `sequence` | 非空 `Last-Event-ID` 优先，其次 query |
| `/v1/eos/executions/{id}/state:watch?afterRevision=N` | `skill-execution-state` | state `revision` | 非空 `Last-Event-ID` 优先，其次 query |
| `/v1/eos/executions/{id}/journal:follow?afterJournalSequence=N` | `skill-journal-entry` | `journalSequence` | 非空 `Last-Event-ID` 优先，其次 query |
| `/v1/eos/executions:monitor?skillName=NAME&skillVersion=VERSION&status=QUEUED,RUNNING&afterMonitorSequence=N` | `skill-monitor-update` | `monitorSequence` | 非空 `Last-Event-ID` 优先，其次 query |
| `/v1/eos/workflows:watch?cursor=epoch:sequence` | `action-list-event` | `epoch:sequence` | 非空 `Last-Event-ID` 优先，其次 query |

每个 `data:` 是一条 ProtoJSON message；`: heartbeat` comment 不携带状态。 非空 `Last-Event-ID` 是重连时的权威输入；它缺失时才使用 query cursor。 客户端应保存最后完整处理的 ID 并用它重连。初始空 monitor reset 可能仅为 `{"reset":true}`，缺失的 sequence 与数组分别表示 `"0"` 与 `[]`。独立 peer-disconnect monitor 会及时取消 upstream gRPC context，即使 stream 正处于空闲；HTTP write failure 是第二条取消路径。north HTTP 直接在有界请求生命周期内写入，不建立无界消息队列。

Workflow cursor 由 opaque non-empty epoch 与 unsigned sequence 组成。只在完整应用 ProtoJSON `ActionListEvent` 后保存 SSE `id`；忽略不大于 durable cursor 的 duplicate，并要求下一条为同 epoch 且 sequence 正好加一。epoch mismatch 或 future cursor 返回 `FAILED_PRECONDITION`，早于 retention window 返回 `OUT_OF_RANGE`；两者都要求重新读取 runtime snapshot，并从新 `actionListCursor` watch。缺失或 malformed cursor 返回 `INVALID_ARGUMENT`。

SSE response 开始后，stream failure 通过 typed `event: error` 返回，其 `data` 使用 下述 error JSON。Unary failure（包括 upstream unavailable）使用映射后的 HTTP status； 已返回 `200` 的 SSE 则在 stream 内报告错误。

## 审计路由

可选 ActionList audit 是本地、外部、非权威投影，与 WebUI 复用同一个 `Client` 和 `WorkflowService` stream。EOS core 从不读取 journal，audit failure 不能修改或停止 core execution。两个 audit route 都设置 `Cache-Control: no-store`。

`GET /v1/eos/audit/status` 返回 `enabled`、`health`、`running`，启用时还返回稳定的 `error`、`cursor` 与 retained-record `records`。health 为 `stopped`、`healthy` 或 `degraded`，error 为 `none`、`invalid_config`、`io`、`corrupt_chain` 或 `resource_exhausted`。禁用时返回 `{"enabled":false,"health":"disabled","running":false}`。

`GET /v1/eos/audit/action-lists` 返回 `{enabled, records, truncated}`；audit disabled 时返回 empty list 与 `truncated: false`。`limit` 默认为 100，范围 1–1000。server 在固定 4 MiB budget 内测量 encoded records JSON array、选择一个 contiguous newest suffix，并按 chronological oldest-first 返回；`truncated` 表示较旧的 requested record 被省略。若 newest record 单独超过 budget，则返回 HTTP `429` / `RESOURCE_EXHAUSTED` 与 `newest audit record exceeds response byte limit`。record 只包含文档允许的 workflow/action/execution/command/device/target ID、enum status、timestamp、revision、cursor、chain field 与少量 fact timing/source-sequence field；绝不包含 argument、result、reason、schema、request body、authentication data、raw protobuf 或 fact key/value/source/frame/unit。operational ID 仍属敏感信息。

append-only journal 位于派生 state directory，使用 owner-only permission、rotation、atomic cursor checkpoint 与 SHA-256 chain。该 chain 只提供 corruption evidence，无法防御能同时改写 record 与 hash 的攻击者，因此不提供 authenticity。精确 storage、recovery、resume、privacy 与 degradation 契约见 [audit module](../../ext/north_http/audit_cn.md)。

## 错误、CORS 与 request ID

JSON 错误格式固定为：

```json
{"code":14,"message":"EOS gRPC upstream is not ready","requestId":"..."}
```

`code` 是数字 gRPC status。主要 HTTP 映射为：invalid argument `400`、 unauthenticated `401`、permission denied `403`、not found `404`、already exists/aborted `409`、failed precondition `412`、resource exhausted `429`、 unimplemented `501`、unavailable `503`、deadline exceeded `504`、data loss `502`；client cancellation 使用 `499`，意外错误使用 `500`。未知路由返回 `404`，过大 body 返回 `413`，超过 active-request 上限返回 `429`。

客户端最多发送一个由 1–96 个 ASCII letter、digit、`.`、`_`、`:` 或 `-` 组成的 `X-Request-ID`。无效值或重复 header 返回 `400`，缺省时自动生成。north HTTP 在 response header 和 error 中返回该 ID， 通过 gRPC metadata 传给 EOS，并绑定到 unary、stream 与异步 skill worker log。 日志只记录 route/RPC、status、duration、byte count、readiness 与 cancellation， 不记录 request body、credential、media、transcript 或 skill payload。

CORS 使用精确 origin allowlist。允许的 origin 可使用 `GET, POST, OPTIONS` 以及 `Content-Type`、`Last-Event-ID`、`X-Request-ID` header，并可读取 response 中的 `X-Request-ID`；不在列表中的 origin 不会收到 allow-origin header。

## 运维与迁移

日志分别写入 `var/log/eos/runs/<launch-id>/<service>/`，每个 service 包含 `events.jsonl` 和直接写入 service directory 的路由日志。retained mode 使用唯一 ID，overwrite mode 复用 `current`，`var/log/eos/latest` 指向最新 run。默认 64 MiB rotation， 保留五份。可在 north HTTP 与 EOS event 文件中查询同一 `correlation_id`。

root process composition 拥有 process start/stop/failure record，server 拥有 listener/upstream/request/stream transport record，shared client 有意不拥有 request logging 或 context policy。该拆分不改变公开 error envelope 或 correlation behavior。

diagnostic event 与 durable audit journal 的 ownership 分离。`north-http.audit` diagnostic 路由到 `north-http/ext-north-http-audit.log` 和进程 `events.jsonl`；`north-http.client` 预留 `north-http/ext-north-http-client.log` route，但当前不产生 event。`[log]` 的 rotation、overwrite、deduplication 与 attachment 设置从不修改 audit journal。使用 [debugging runbook](../../../usage/debugging_cn.md)关联 request ID、Workflow cursor、gRPC failure 与 audit recovery，且不要采集已排除的 payload。

旧 `[gateway].json_port` 与 `eosd --json-port` 已直接移除。把 HTTP port 与 CORS 配置迁移到 `north-http.toml`，单独启动 north HTTP，并让 WebUI/REPL 指向 它。生产 service manager 应分别管理 `eosd`、`eos-north-http` 与 simulator； `eosd` 永不启动这些外部进程。

可执行客户端与 guided parameter 流程见 [REPL 指南](../../ext/tools/repl_cn.md)。

# gRPC 进程接口

[Interface overview](README_cn.md) links the contract surfaces and runtime owners.

这是 Gateway、Skill 与 Workflow 三个 protobuf service 的 wire 说明。EOS core 只提供 gRPC；外部 adapter 根据 annotation 提供 REST/SSE。EOS 自有并 自动注册设备与 primitive。

三个 service 共用一个 EOS gRPC listener。`eos-north-http` 是独立进程， 为 Gateway、Skill、Workflow unary 提供 ProtoJSON REST，并把三个 server stream 映射为 SSE。`eos-simulator` 的 `16004` WebUI API 是另一接口。

The wire layers are fixed:

| Layer | Authority | Purpose |
| --- | --- | --- |
| gRPC envelope | `gateway.proto`, `skills.proto`, `workflow.proto` | RPC names, fields, status, streaming |
| JSON payload | JSON Schemas under `share/ext/eos/` | Dynamic arguments, acknowledgements, and resources; private model schemas are under `share/contracts/eos/external/models/schemas/` |
| HTTP/SSE façade | protobuf `google.api.http` annotations | external adapter route authority |

HTTP is not a separate REST contract. Field names and validation remain identical to the gRPC/ProtoJSON representation.

各接口的专题文档提供更聚焦的 payload 示例：[Body](body_cn.md)、[World](world_cn.md)、[Primitive](actions_cn.md)、[SkillService](skills_cn.md) 与 [Workflow](workflow_cn.md)。

## 读取、计算和写入准入

gRPC 服务是 EOS 自有领域服务的适配层。发现、状态、资源和历史读取、watch、导航读取及技能/工作流查询使用 EOS 的投影和保留存储，不会为公开读取同步调用硬件驱动。EOS 按运行节奏采样硬件本体数据；需要该数据的读取在样本缺失、不健康或超过一秒时失败。成功的本体读取通过 gRPC 初始元数据 `eos-sample-age-ms` 返回样本年龄（毫秒）；HTTP 适配层将其转发为 `Eos-Sample-Age-Ms`，无需改变资源 JSON Schema。读取仍校验资源身份、新鲜度、范围和响应大小。

相机采集和感知推理是受限计算调用。它们可以使用已校验快照并调用模型提供方，受到截止时间、取消和容量限制，不能作为无限制缓存读取。

primitive 调用及取消、航点变更、技能执行和 SIGNAL、工作流提交属于写入。gRPC handler 先调用所属领域的准入路径，校验 schema、能力、目标、安全策略、幂等性和队列，再改变状态。handler 不直接调用硬件 HAL。传输 request ID 用于日志关联；操作自己的 command/request ID 遵循各自的重放规则。

部署策略把可以访问本地监听器的客户端视为可信。回环 TCP 无法区分本机用户；同机客户端优先使用可选的、由所有者控制权限的 Unix 域套接字。逐客户端授权需要单独设计；写入路径没有放行用的安全占位实现。

## 1. Transport health

The default listeners are plaintext and unauthenticated on loopback:

| Interface | Configuration | Default |
| --- | --- | --- |
| native gRPC | `[gateway] grpc_port`, `--grpc-port` | `127.0.0.1:16003` |
| HTTP/ProtoJSON/SSE | `[north_http] port` in `north-http.toml` | `127.0.0.1:16005` |

gRPC health/reflection 属于 EOS。adapter `/health/live` 表示进程存活， `/health/ready` 仅在 upstream gRPC serving 时成功。

每个 EOS process 绑定一个 hardware profile 与一个 gRPC endpoint；需要浏览器 访问时配套独立 adapter listener。额外 robot 使用额外 endpoint。

## 2. Runtime initialization and status

### Discover

```proto
rpc Discover(google.protobuf.Empty) returns (DiscoverResponse);
```

HTTP: `GET /v1/eos/discover`

Agent 连接后立即调用 `Discover`。响应只包含精简机器人信息，以及每个设备的 `id`、`class`、`part`、`state_uri`、`state`、`health` 和已接纳的 `primitives`。`state_uri` 指向该设备的统一可读状态资源。机器人 descriptor 的 `primitives` 发布空 `deviceId` 的 `robot.stop` 与 `safety.estop`；base、arm 与 gimbal 各自发布隐式 `motion.stop`。每个 primitive binding 包含精确 name/version、`direct` 或 `advanced` kind、允许的 `relative`/`absolute` references、limits、health、dependency health、`parameters` 参数 JSON Schema，以及需要时的 absolute frame。客户端不能添加、替换或删除设备或 primitive。

`parameters` 描述该 binding 的 caller-owned `arguments` object：它移除 EOS 注入的 `type` 与 `command_id`，按设备收窄 reference/frame、固定 joint array 长度，并移除 profile 不支持的可选数值参数。动态数值运行范围仍单独保留在 `limits`，客户端应把它作为提示，不要把 relative-motion 范围误写成静态 schema bound。

设备 `state` 来自健康与遥测：`disabled`、`idle`、`active` 或 `fault`；当现有遥测不足以证明活动或静止时为 `unknown`。传感器也可报告 `capturing` 或 `listening`。

Device IDs are deterministic, human-writable `<class>-<zero-based-index>` values. Indices are independent per class and follow stable binding order, for example `arm-0`, `base-0`, `arm-1`.

```json
{
  "robot": {"id":"demo","name":"demo","model":"eos-demo","state":"idle","health":"ready","estop":false},
  "devices": [
    {
      "id": "base-0",
      "class": "base",
      "part": "default",
      "state_uri": "robots/demo/devices/base-0/state",
      "state": "idle",
      "health": "ready",
      "primitives": [{"name":"base.navigate","version":"1.0.0","kind":"direct","references":["relative","absolute"],"absoluteFrame":"map","health":"ready","parameters":{"type":"object","properties":{"reference":{"type":"string","enum":["relative","absolute"]},"frame_id":{"type":"string","enum":["map"]},"x_m":{"type":"number"},"y_m":{"type":"number"},"yaw_rad":{"type":"number"}},"required":["reference","x_m","y_m","yaw_rad"],"additionalProperties":false}}]
    }
  ]
}
```

### GetStatus

```proto
rpc GetStatus(google.protobuf.Empty) returns (google.protobuf.Struct);
```

HTTP: `GET /v1/eos/status`

`GetStatus` remains the compatible ongoing status API. It returns `robot_id`, aggregate status, EOS version, start time, uptime, a correlation ID, subsystem status, device counts, and the single-runtime robot overview. Unlike standard gRPC health, it may report `degraded` while the transport remains live.

## 3. Resource discovery, reads, and watches

### ListResources

```proto
rpc ListResources(google.protobuf.Empty) returns (google.protobuf.ListValue);
```

HTTP: `GET /v1/eos/resources`

响应是可读取的 `{class, uri}` 列表。硬件清单由 `Discover` 返回；每个受支持的 admitted 设备状态也作为资源暴露。机器人范围资源使用配置中的 `robot_name`：

```text
robots/{robot_name}/detection/state
robots/{robot_name}/world/asr/state
robots/{robot_name}/devices/{device_id}/state
```

设备状态读取不需要 model producer，并通过现有 polling watch。`robots/{robot_name}/arm/state` 继续作为详细机械臂 telemetry 的兼容 alias。语义世界资源只有在对应生产者已经提供通过契约校验的结果后才会出现；EOS 不伪造空观测。聚合运行时和机器人状态由 `GetStatus` 返回，不是可读取资源。

### ReadResource

```proto
rpc ReadResource(ResourceRequest) returns (google.protobuf.Struct);
```

HTTP: `GET /v1/eos/resource?uri=...`

`ResourceRequest` contains the required, non-empty `uri`. The response mapping is:

| URI | Payload contract |
| --- | --- |
| `robots/{robot_name}/detection/state` | `perception/world/detection.v1.json` (`detection.snapshot`) |
| `robots/{robot_name}/world/asr/state` | `world/asr.v1.json` (`asr.snapshot`) |

规范 `world.snapshot` schema 保留给未来的真实 fusion producer；当前 runtime 不发布或路由 `robots/{robot_name}/world/state`。

快照包含非负序列号、RFC 3339 时间戳及各自的规范化语义数据。`detection.snapshot.sequence` 标识按需读取的 resource envelope；当 camera pose model 提供检测结果时，`source_sequence` 仅在新的 inference result 存入后递增。需要等待运动后新画面的 skill 必须同步 `source_sequence`，不能使用每次读取 都会变化的 envelope sequence。相机模型快照可携带源图像宽高以及由左、上、右、下像素坐标组成的有界 `bbox`，供经过审核的视觉伺服 skill 使用；PCM、JPEG、关键点、mask 和模型 tensor 不通过资源接口暴露。

### WatchResource

```proto
rpc WatchResource(ResourceRequest) returns (stream ResourceChanged);
```

EOS gRPC stream 本身在内容未变化时保持安静。外部 adapter 在 `GET /v1/eos/resource:watch?uri=...` 提供 `resource-changed` SSE，并按配置发送 heartbeat comment。每条 change 只含 URI；客户端随后用 `ReadResource` 读取 完整 snapshot。HTTP disconnect 会取消 upstream gRPC stream。

## 4. Primitive discovery

Primitive discovery 是 `Discover.devices[].primitives` 的一部分，不存在独立 registry RPC。EOS 在 init 时从内置 schema 与 deployment profile 自动注册、绑定并接纳 primitive。Northbound 只能发现与调用，不能 mutation。

## 5. SkillService discovery

Skills are deployment-owned native code libraries loaded at EOS startup and referenced by the exact pair `{name, version}`. Each library embeds its metadata and the complete allowlist of existing primitives its code may request. EOS does not author skills; WorkflowService validates and schedules caller-supplied ActionList definitions.

### ListSkills and GetSkill

```proto
rpc ListSkills(ListSkillsRequest) returns (ListSkillsResponse);
rpc GetSkill(GetSkillRequest) returns (SkillDescriptor);
```

EOS 通过 gRPC 提供；adapter 映射为 `GET /v1/eos/skills` 与 `GET /v1/eos/skills/{name}/versions/{version}`。

`ListSkills` 返回按精确 name/version 排序的完整 profile-neutral descriptor；`GetSkill` 查询一个精确身份。Descriptor 包含说明、参数/返回 schema、primitive dependency、可读 resource template、分开的接收/发出 SIGNAL 声明、claim template、声明的 inference authority 与 `reentrant` 标志。Inference authority 是该 skill 获准使用的 typed `VAD`、`HUMAN`、`HUMAN_POSE` 与 `FACE_RECOGNITION` 值，不公开 model ID。Descriptor 不暴露 library/build/provider metadata 或 primitive acknowledgement。未配置技能时 list 为空；未知身份返回 `NOT_FOUND`。

## 6. SkillService execution

`StartExecution` requires an exact skill reference, object arguments, a 1–96 byte idempotency `request_id`, and a positive `google.protobuf.Duration` no greater than one hour. It returns the first complete `SkillExecution` snapshot. An identical retained request ID returns the existing execution; reuse with different content returns `ALREADY_EXISTS`. Start request IDs are manager-wide; EOS derives its private root primitive command ID from the request ID and never exposes command IDs or primitive acknowledgements through this service.

`GetExecution` 返回当前 snapshot。`WatchExecution` replay sequence 大于 `after_sequence` 的 retained snapshot，随后 follow live transition；若 execution 已经终止且 `after_sequence` 大于等于其 terminal sequence，stream 会成功关闭且不发送 event。adapter 使用 REST 与 `skill-execution` SSE 暴露这些操作。断开 stream 不取消 execution。

Snapshot 包含 execution/request ID、精确 skill reference、`runtime_id`、typed status、生命周期 `sequence`、统一的 `latest_journal_sequence`、termination flag、可选 `queue_reason`、已解析 claim、reason code、有界 diagnostic message 与 result。`sequence` 是 `WatchExecution.after_sequence` 的 cursor；`latest_journal_sequence` 是 `FollowExecutionJournal.after_journal_sequence` 的 cursor。入站 SIGNAL 入队本身不生成 journal entry。状态从 `EXECUTION_STATUS_QUEUED` 到 `EXECUTION_STATUS_RUNNING`，终态为 `SUCCEEDED`、`REJECTED`、`CANCELLED`、`TIMED_OUT` 或 `FAILED`。

reason code 是稳定 automation contract；client 不得解析 message。message 是可选、 经过清理且最多 1,024 bytes 的 operator text，并通过 Get/List/Watch 与 north HTTP/SSE 原样传递。

`SkillManager` 默认使用 4 个 worker，最多保留 1024 条进程内 execution 记录、每个 execution 256 个待处理入站 SIGNAL 和 1024 个统一 event。达到记录上限时先淘汰最旧的可淘汰终态记录，否则拒绝新任务。记录、event、SIGNAL 队列与幂等保证均只存在于当前进程。

### SIGNAL 扩展

运行时机制见 [技能运行机制](../../design/runtime/skills_cn.md)。

服务增加三个操作：`ListExecutions` 返回所有仍保留 execution 的当前 snapshot，提供 类似进程管理器 `ps` 的视图；`SignalExecution` 接收目标 execution ID、幂等 request ID、已声明 SIGNAL 名称与 object payload，并返回只表示验证和入队成功的回执；`FollowExecutionJournal` 通过同一 execution 级 cursor 重放并跟随生命周期 snapshot、出站 SIGNAL 或 journal record。现有 `WatchExecution` 保持为只含 生命周期的兼容 stream。

`SkillSignal` 携带 manager 生成的 signal ID、execution ID、方向、小写点分名称、event sequence 与 object payload。目标 descriptor 以 JSON Schema 分别声明接受与发出的 SIGNAL；入队或发布前验证方向、 声明和 payload。`CANCEL` 是 manager 保留的空 object SIGNAL，保持协作式取消语义； 终止使用协作式 `TerminateExecution`；不提供 POSIX signal number 或 hard kill。

Descriptor 还公开绑定到调用字段的 `shared_read` 与 `exclusive_use` claim template。Template 使用以 `/` 开头的 RFC 6901 JSON Pointer，Start 时必须解析为非空字符串。只有 kind 与 identity 同时相同且至少一方为 `exclusive_use` 时才冲突。 `ListExecutions`、`GetExecution` 与 execution event 公开解析后的 claim 及可选 `queue_reason`； `claim.conflict` 表示 execution 保持 `queued`，直到完整 claim 集合可被原子获取。兼容 claim 可并行运行， 共享 claim 的 execution 之间保持 FIFO 顺序。

应用 SIGNAL 可以在 execution 为 `queued` 时排队，按 execution 有序且只存在于进程内。仍保留的相同 `(execution_id, request_id)` 返回原回执，不同内容复用时返回 `ALREADY_EXISTS`。出站 SIGNAL 与生命周期 transition 共用 一个有界可重放 cursor；重连时传入上一个已处理 sequence 可避免重复。`FollowExecutionJournal` 不回显入站 SIGNAL，并在发送终态 snapshot 后关闭。skill diagnostic log 只进入受保护、仅 operator 可见的 `skills` logger，不进入公共 journal。外部 Agent 是唯一 router；EOS 不直接在 execution 间转发，也不 提供 topic 或 broadcast。

已实现的 SIGNAL 错误映射对发往终态 execution 的应用 SIGNAL 使用 `FAILED_PRECONDITION`；格式错误、 非目标技能接收声明中的名称或不符合 schema 使用 `INVALID_ARGUMENT`；execution 未保留使用 `NOT_FOUND`；幂等键 冲突使用 `ALREADY_EXISTS`；有界 queue/history 满使用 `RESOURCE_EXHAUSTED`；provider 不支持 SIGNAL 使用 `UNAVAILABLE`；协议不变量被破坏使用 `INTERNAL`。

`MonitorExecutions` 提供类似 `htop` 的全局视图。selector 可指定精确 skill name/version 与 status 集；仅 version 无 name 非法，空 status 默认匹配 `QUEUED` 和 `RUNNING`。新连接收到 `reset=true` 的完整 snapshot；有效 cursor 重放 delta，过期 cursor 收到新 reset，超前 cursor 被拒绝。update 使用进程内单调 `monitor_sequence`，携带完整 upsert 和离开 selector 或被淘汰的 execution ID。

`TerminateExecutions` 提供类似 `pkill` 的批量协作式终止。它要求精确 skill name 与 request ID，可选精确 version，只原子选择 queued/running execution，并返回排序后的固定目标 ID。相同 request ID 与 selector 重试返回原目标（包括空集）；冲突复用返回 `ALREADY_EXISTS`，重试不会捕获后来启动的 execution。

## 7. Direct primitive release

### CallPrimitive

```proto
rpc CallPrimitive(CallPrimitiveRequest) returns (google.protobuf.Struct);
```

HTTP: `POST /v1/eos/primitives:call` with a ProtoJSON body.

`CallPrimitiveRequest` contains the exact primitive `name` and `version`, explicit `device_id`, and dynamic `arguments`. Device primitives require a non-empty target; process-wide `safety.estop` requires an empty target. The transport-neutral Dispatcher validates arguments against the selected primitive schema, admits the target, and performs the controlled backend release; gateway maps its result. State or safety rejection is a successful RPC with `accepted: false`, not a transport error.

可调用 surface 包含十五个与 backend 对齐的 primitive：

| Primitive | Tier | Target | Required arguments |
| --- | --- | --- | --- |
| `base.velocity` | standard | `base-*` | `linear_x_m_s`, `linear_y_m_s`, `angular_z_rad_s`, and integer `valid_for_ms` within the selected device's advertised limits |
| `base.navigate` | standard | `base-*` | `reference`、`x_m`、`y_m`、`yaw_rad`；absolute 时需要 `frame_id` |
| `base.posture` | standard | `base-*` | `posture` (`stand`, `walk`, or `lie`) |
| `gimbal.motion` | standard | `gimbal-*` | `reference`、`pan_rad`、`tilt_rad`，可选 `speed_rad_s` |
| `arm.init` | operator | `arm-*` | none; releases the selected device's configured initial joint pose |
| `arm.movej@1.0.0` | operator | `arm-*` | `reference`、完整 `joint_positions_rad`，可选 `speed_rad_s` |
| `arm.movel@1.0.0` | operator | `arm-*` | `reference`、`position_m`、`orientation_quat_xyzw`；absolute 时需要 `frame_id` |
| `arm.pick@1.0.0` | standard | `arm-*` | 当前 world/detection 中符合 `obj:*` 格式的 `object_id` |
| `gripper.motion` | standard | `gripper-*` | `action`（`open` 或 `close`），可选 `force_n` |
| `speaker.play@1.0.0` | standard | `speaker-*` | EOS 可读的音频 asset `uri` |
| `speaker.speak` | advanced | `speaker-*` | `text`，可选 `language` 与 `voice` |
| `face.register@1.0.0` | advanced | `cam-*` | 非空、最多 256 个字符的 `name` |
| `motion.stop` | privileged | base, arm, or gimbal | none |
| `robot.stop` | privileged | empty device ID | none |
| `safety.estop` | privileged | empty device ID | none |

`robot.stop` 并行请求所有 motion-capable part 受控停止，等待每个 backend 的终态确认，并在 `result.parts` 中聚合各 device 的结果；任一 part 未确认停止时整体不成功。它可幂等重放，但不会接合 emergency-stop latch。

顶层 `reason` 与 `reason_code` 仅用于准入拒绝。已准入命令即使终态为 `failed`、`cancelled`、`timed_out` 或 `outcome_unknown`，这两个顶层字段也保持 null，详情位于 `result`。`commandId` 必须全局唯一，EOS 或下游 controller 重启后也不得用于另一条命令；durable journal 会重放已记录终态，并把只有 reservation 的恢复记录标为 `outcome_unknown`，绝不再次 release。

Example:

```json
{
  "name": "base.velocity",
  "version": "1.0.0",
  "deviceId": "base-0",
  "commandId": "move-42",
  "arguments": {
    "linear_x_m_s": 0.3,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.1,
    "valid_for_ms": 250
  }
}
```

所有 primitive schema 拒绝未知字段；schema 内的 nullable `command_id` 与匹配的 `type` discriminator 由 EOS 根据 envelope 注入，northbound 调用者使用顶层 `commandId`。缺失或未知版本 fail closed。Portable 结构约束位于 action schema；硬件相关的 reference 模式与范围来自选定设备的 discovery binding，并在 backend release 前执行。参见 [Primitive Payload Contracts](actions-payloads_cn.md)。

## 8. 按需相机捕获

`CaptureFrames` 通过 `POST /v1/eos/world/camera:capture` 接受机器人名、相机设备 ID 和 `1` 或 `2` 的帧数。响应是带 JPEG 字节、尺寸与 capture time 的 `vision.snapshot`；相机的 body discovery 仍只包含 health/state。缓存不足、设备错误或不健康时返回 `UNAVAILABLE`。

人物 detection 的 `attributes.name` 表示人脸识别结果。Provider 标记为 known 且 similarity 至少为 `0.40` 时输出注册姓名；只要同一个显式且唯一的 track ID 仍然存在，EOS 就会忽略 bbox 移动，在所有后续空、unknown 或低 similarity 结果中保留该姓名。没有可保留身份的未识别人脸输出 `"unknown"`；没有安全匹配身份的人物输出 `null`。不同 track、重复 ID 或目标消失都会清除缓存姓名。

人脸注册通过相机上的 advanced primitive `face.register@1.0.0` 完成。调用 `CallPrimitive`，以相机 ID 为 `deviceId`，并在 `arguments.name` 中提供姓名。 EOS 使用相机当前有效帧调用该 primitive 绑定的 `vision.face` 模型；EOS 不返回或持久化图片，注册身份由模型 provider 保存。

模型调用与健康检查是 EOS 和模型提供者之间的私有接口。Agent 侧没有模型发现、模型调用或模型状态资源；仅在 `GetStatus` 中看到聚合健康。

## Complete service order

```proto
service GatewayService {
  rpc Discover(google.protobuf.Empty) returns (DiscoverResponse);
  rpc GetStatus(google.protobuf.Empty) returns (google.protobuf.Struct);
  rpc ListResources(google.protobuf.Empty) returns (google.protobuf.ListValue);
  rpc ReadResource(ResourceRequest) returns (google.protobuf.Struct);
  rpc WatchResource(ResourceRequest) returns (stream ResourceChanged);
  rpc CallPrimitive(CallPrimitiveRequest) returns (google.protobuf.Struct);
  rpc CancelPrimitive(PrimitiveCommandRequest) returns (google.protobuf.Struct);
  rpc CaptureFrames(CaptureFramesRequest) returns (CaptureFramesResponse);
}

service SkillService {
  rpc ListSkills(ListSkillsRequest) returns (ListSkillsResponse);
  rpc GetSkill(GetSkillRequest) returns (SkillDescriptor);
  rpc StartExecution(StartExecutionRequest) returns (SkillExecution);
  rpc GetExecution(GetExecutionRequest) returns (SkillExecution);
  rpc WatchExecution(WatchExecutionRequest) returns (stream SkillExecution);
  rpc TerminateExecution(TerminateExecutionRequest) returns (SkillExecution);
  rpc TerminateExecutions(TerminateExecutionsRequest) returns (TerminateExecutionsResponse);
  rpc ListExecutions(ListExecutionsRequest) returns (ListExecutionsResponse);
  rpc SignalExecution(SignalExecutionRequest) returns (SkillSignalReceipt);
  rpc FollowExecutionJournal(FollowExecutionJournalRequest) returns (stream SkillJournalEntry);
  rpc MonitorExecutions(MonitorExecutionsRequest) returns (stream SkillMonitorUpdate);
}

service WorkflowService {
  rpc ApplyActionList(ApplyActionListRequest) returns (ActionList);
  rpc ListActionLists(ListActionListsRequest) returns (ListActionListsResponse);
  rpc GetActionList(GetActionListRequest) returns (ActionList);
  rpc GetRuntimeSnapshot(GetRuntimeSnapshotRequest) returns (ManagerRuntimeSnapshot);
}
```

## Error mapping

| Condition | gRPC status | HTTP status |
| --- | --- | --- |
| malformed envelope, missing required field, or invalid primitive arguments | `INVALID_ARGUMENT` | `400` |
| unknown resource, primitive, skill, or camera identity | `NOT_FOUND` | `404` |
| conflicting retained skill request ID | `ALREADY_EXISTS` | `409` |
| application SIGNAL targets a terminal execution | `FAILED_PRECONDITION` | `412` |
| execution record, SIGNAL queue, or event capacity exhausted | `RESOURCE_EXHAUSTED` | `429` |
| required composed service or SIGNAL capability unavailable | `UNAVAILABLE` | `503` |
| camera cache does not contain the requested distinct frames | `UNAVAILABLE` | `503` |
| skill completion or primitive state rejection | `OK` | `200` |
| internal failure | `INTERNAL` | `500` |

ProtoJSON uses protobuf JSON names。HTTP error 包含 numeric `code`、`message` 与 `requestId`；完整 mapping/SSE reconnect/CORS 见 [adapter contract](external/http_cn.md)。

## Rendered contract

From a source checkout, print the deterministic service description and canonical dynamic schemas:

```sh
./scripts/print_contract_interfaces.py --contracts /path/to/share
```

The script resolves contracts from `--contracts`, then `EOS_CONTRACTS`, then `ISLAND_SHARE`, and finally the repository `share` tree. Runtime startup uses the first three sources only and requires an existing absolute root.

## Related

- [Gateway implementation](../../design/runtime/northbound_cn.md)
- [Contracts](contracts_cn.md)
- [Diagnostic REPL](../ext/tools/repl_cn.md)
- [Native skill runtime](../../design/runtime/skills_cn.md)
- [External decision client](../../../../src/eos/ext/decision/README.md)

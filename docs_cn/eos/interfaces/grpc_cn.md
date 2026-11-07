# gRPC 进程接口

[English](../../../docs/eos/interfaces/grpc.md)

**实现状态：** 默认 Rust EOS 运行时现已提供标准 gRPC health，以及下文[已合并 R02 子集](#r02-冻结的-rust-子集)列出的 11 个 Gateway/CapabilityService RPC。[PR #314](https://github.com/Island-Team/Island-eos/pull/314)在独立评审后合并；[本地资格验证证据](../develop/grpc-gateway_cn.md)覆盖精确子集。下文 EOS R03 受管理准入代码已通过 [PR #315](https://github.com/Island-Team/Island-eos/pull/315) 合并，IsOS 授权交付已通过 [PR #52](../../island/retired-repository-references_cn.md#ref-aee8e4a3d593) 合并。合并后全新主目录重放验收 R03；R05/R06 与限定范围 M3 完成条件随后通过验收；见[M3 记录](../../island/m3-qualification_cn.md)。已发布 protobuf 契约与可选 C++ 服务仍作为历史兼容接口。当前 Python EGIA 客户端要求健康、Gateway 发现/资源/状态及 CapabilityService 能力发现通过后才报告就绪。

**R04 实现切片：** 默认 Rust listener 已增加基于 IsOS 已选择原生宿主目录的[七方法 SkillService 子集](skills_cn.md#r04-默认-rust-子集实现切片)。Start/Terminate 沿用 R03 的 `authorization` 和 `x-eos-active-revision` metadata，不改 `skills.proto`，声明的 primitive 仍经同一 Dispatcher。`host-skill-abi-v2` feature 表示仅 primitive 的描述符准入以及必需的 Rust worker/Broker 强制边界；声明 resource、source、inference、SIGNAL 或 community callback 的描述符在相应所有者完成前整体拒绝。其他 SkillService 方法和 WorkflowService 仍不可用。

下文直到“监听端口”描述已发布 C++ 服务及 HTTP 投影，作为兼容参考；这不表示 Rust 运行时或当前 HTTP bridge 已实现全部历史 RPC 或 REST/SSE 路由。末节规定已实现的 Rust 子集及其生命周期。

[Interface overview](README_cn.md) links the contract surfaces and runtime owners.

这是 Gateway、Skill、Workflow 与新增 Capability 四个 protobuf service 的 wire 说明。EOS core 只提供 gRPC；外部 adapter 提供受支持的 v1 REST/SSE 与 `/v2/eos` 能力操作 REST/SSE。EOS 自有并自动注册设备与 primitive。

历史可选 C++ 服务在一个 EOS gRPC listener 上提供这四个 service；当前 Rust listener 只提供下文 R02 子集。独立 `eos-north-http` 的历史契约为受支持的 Gateway、Skill、Workflow unary 提供 ProtoJSON REST，并将其支持的 server stream 映射为 SSE；这些历史路由不代表当前 Rust HTTP bridge 已全部实现。MuJoCo 的私有 socket 由外部软件包提供，不是 EOS listener。

The wire layers are fixed:

| Layer | Authority | Purpose |
| --- | --- | --- |
| gRPC envelope | `gateway.proto`, `skills.proto`, `workflow.proto`, `capabilities.proto` | RPC names, fields, status, streaming |
| JSON payload | JSON Schemas under `share/eos/schemas/eos/` | Dynamic arguments, acknowledgements, and resources; private model schemas are under `share/eos/contracts/eos/external/models/schemas/` |
| HTTP/SSE façade | protobuf `google.api.http` annotations | external adapter route authority |

HTTP is not a separate REST contract. Field names and validation remain identical to the gRPC/ProtoJSON representation.

各接口的专题文档提供更聚焦的 payload 示例：[Body](body_cn.md)、[World](world_cn.md)、[Primitive 参数与语义](actions_cn.md)、[CapabilityService v2](capabilities_cn.md)、[SkillService](skills_cn.md) 与 [Workflow](workflow_cn.md)。

## 读取、计算和写入准入

gRPC 服务是 EOS 自有领域服务的适配层。发现、状态、资源和历史读取、watch、导航读取及技能/工作流查询使用 EOS 的投影和保留存储，不会为公开读取同步调用硬件驱动。EOS 按运行节奏采样硬件本体数据；需要该数据的读取在样本缺失、不健康或超过一秒时失败。成功的本体读取通过 gRPC 初始元数据 `eos-sample-age-ms` 返回样本年龄（毫秒）；HTTP 适配层将其转发为 `Eos-Sample-Age-Ms`，无需改变资源 JSON Schema。读取仍校验资源身份、新鲜度、范围和响应大小。

相机采集和感知推理是受限计算调用。它们可以使用已校验快照并调用模型提供方，受到截止时间、取消和容量限制，不能作为无限制缓存读取。

primitive 调用及取消、航点变更、技能执行和 SIGNAL、工作流提交属于写入。gRPC handler 先调用所属领域的准入路径，校验 schema、能力、目标、安全策略、幂等性和队列，再改变状态。handler 不直接调用硬件 HAL。传输 request ID 用于日志关联；操作自己的 command/request ID 遵循各自的重放规则。

部署策略把可以访问本地监听器的客户端视为可信。回环 TCP 无法区分本机用户；同机客户端优先使用可选的、由所有者控制权限的 Unix 域套接字。逐客户端授权需要单独设计；写入路径没有放行用的安全占位实现。

## 1. Transport health

The default listeners are plaintext and unauthenticated on loopback:

| Interface | Configuration | Default |
| --- | --- | --- |
| native gRPC | `[gateway] grpc_port`, `--grpc-port` | `127.0.0.1:16200` |
| HTTP/ProtoJSON/SSE | `[north_http] port` in `north-http.toml` | `127.0.0.1:16201` |

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

运行时机制见 [技能运行机制](../design/runtime/skills_cn.md)。

服务增加三个操作：`ListExecutions` 返回所有仍保留 execution 的当前 snapshot，提供 类似进程管理器 `ps` 的视图；`SignalExecution` 接收目标 execution ID、幂等 request ID、已声明 SIGNAL 名称与 object payload，并返回只表示验证和入队成功的回执；`FollowExecutionJournal` 通过同一 execution 级 cursor 重放并跟随生命周期 snapshot、出站 SIGNAL 或 journal record。现有 `WatchExecution` 保持为只含 生命周期的兼容 stream。

`SkillSignal` 携带 manager 生成的 signal ID、execution ID、方向、小写点分名称、event sequence 与 object payload。目标 descriptor 以 JSON Schema 分别声明接受与发出的 SIGNAL；入队或发布前验证方向、 声明和 payload。`CANCEL` 是 manager 保留的空 object SIGNAL，保持协作式取消语义； 终止使用协作式 `TerminateExecution`；不提供 POSIX signal number 或 hard kill。

Descriptor 还公开绑定到调用字段的 `shared_read` 与 `exclusive_use` claim template。Template 使用以 `/` 开头的 RFC 6901 JSON Pointer，Start 时必须解析为非空字符串。只有 kind 与 identity 同时相同且至少一方为 `exclusive_use` 时才冲突。 `ListExecutions`、`GetExecution` 与 execution event 公开解析后的 claim 及可选 `queue_reason`； `claim.conflict` 表示 execution 保持 `queued`，直到完整 claim 集合可被原子获取。兼容 claim 可并行运行， 共享 claim 的 execution 之间保持 FIFO 顺序。

应用 SIGNAL 可以在 execution 为 `queued` 时排队，按 execution 有序且只存在于进程内。仍保留的相同 `(execution_id, request_id)` 返回原回执，不同内容复用时返回 `ALREADY_EXISTS`。出站 SIGNAL 与生命周期 transition 共用 一个有界可重放 cursor；重连时传入上一个已处理 sequence 可避免重复。`FollowExecutionJournal` 不回显入站 SIGNAL，并在发送终态 snapshot 后关闭。skill diagnostic log 只进入受保护、仅 operator 可见的 `skills` logger，不进入公共 journal。外部 Agent 是唯一 router；EOS 不直接在 execution 间转发，也不 提供 topic 或 broadcast。

已实现的 SIGNAL 错误映射对发往终态 execution 的应用 SIGNAL 使用 `FAILED_PRECONDITION`；格式错误、 非目标技能接收声明中的名称或不符合 schema 使用 `INVALID_ARGUMENT`；execution 未保留使用 `NOT_FOUND`；幂等键 冲突使用 `ALREADY_EXISTS`；有界 queue/history 满使用 `RESOURCE_EXHAUSTED`；provider 不支持 SIGNAL 使用 `UNAVAILABLE`；协议不变量被破坏使用 `INTERNAL`。

`MonitorExecutions` 提供类似 `htop` 的全局视图。selector 可指定精确 skill name/version 与 status 集；仅 version 无 name 非法，空 status 默认匹配 `QUEUED` 和 `RUNNING`。新连接收到 `reset=true` 的完整 snapshot；有效 cursor 重放 delta，过期 cursor 收到新 reset，超前 cursor 被拒绝。update 使用进程内单调 `monitor_sequence`，携带完整 upsert 和离开 selector 或被淘汰的 execution ID。

`TerminateExecutions` 提供类似 `pkill` 的批量协作式终止。它要求精确 skill name 与 request ID，可选精确 version，只原子选择 queued/running execution，并返回排序后的固定目标 ID。相同 request ID 与 selector 重试返回原目标（包括空集）；冲突复用返回 `ALREADY_EXISTS`，重试不会捕获后来启动的 execution。

## 7. 能力动作

EOS 0.2.0 通过 [CapabilityService v2](capabilities_cn.md) 的 `ListCapabilities`、`SubmitOperation`、`GetOperation`、`WatchOperation` 与 `CancelOperation` 提供公开硬件动作。HTTP 使用 `/v2/eos/capabilities` 和 `/v2/eos/operations` 路由。五个 v1 Gateway primitive 动作 RPC 已退役；下面的 primitive 清单与参数说明仍用于解释硬件契约。

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
  "capabilityName": "hardware/demo/base/default/base.velocity@1.0.0",
  "operationId": "move-42",
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

人脸注册通过相机上的 advanced primitive `face.register@1.0.0` 完成。提交目录中与该相机绑定的能力，并在 `arguments.name` 中提供姓名。 EOS 使用相机当前有效帧调用该 primitive 绑定的 `vision.face` 模型；EOS 不返回或持久化图片，注册身份由模型 provider 保存。

模型调用与健康检查是 EOS 和模型提供者之间的私有接口。Agent 侧没有模型发现、模型调用或模型状态资源；仅在 `GetStatus` 中看到聚合健康。

## Complete service order

```proto
service GatewayService {
  rpc Discover(google.protobuf.Empty) returns (DiscoverResponse);
  rpc GetStatus(google.protobuf.Empty) returns (google.protobuf.Struct);
  rpc ListResources(google.protobuf.Empty) returns (google.protobuf.ListValue);
  rpc ReadResource(ResourceRequest) returns (google.protobuf.Struct);
  rpc WatchResource(ResourceRequest) returns (stream ResourceChanged);
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

- [Gateway implementation](../design/runtime/northbound_cn.md)
- [Contracts](contracts_cn.md)
- [Diagnostic REPL](../develop/ext/tools/repl_cn.md)
- [Native skill runtime](../design/runtime/skills_cn.md)
- [External decision client](../../../adapters/eos-cxx/ext/decision/README.md)

## Rust 0.x 网关——已合并的 R02 子集

Rust 网关使用现有生成的 `island.eos.v1` 和 `island.eos.v2` 包，提供标准 gRPC health 和下表 11 个 RPC。在报告 `SERVING` 之前，EGIA 启动所需的 Gateway `Discover`、`ListResources`、`GetStatus` 与 CapabilityService `ListCapabilities` 均可用。响应只投影同一已准入图代际、机器人身份、节点描述、健康和精确原语绑定，不伪造缺失设备或能力。能力操作映射到 Dispatcher 持久命令 ID 及其已接受、拒绝、终态或不确定结果。其他已发布服务保留文档，但 Rust handler 通过对等测试前不能报告就绪。

只在图、适配器与命令日志准入之后绑定。每次运行启动只发布一个完整、不可变的发现代际。被拒绝的候选不能替换已有 listener 的代际；实时健康可降级而不改变 binding 身份。模拟器缺失、观察过期、绑定未知、schema 不兼容或截止时间过期，均在执行器放行前返回文档规定的类型化状态或规范 gRPC 错误。关闭时先把健康改为 `NOT_SERVING`，停止新的写入，完成或取消有界工作，通过 Dispatcher 停止，再关闭流。保留 protobuf 字段号、ProtoJSON 名称、方法名和 service 包 ID。新增可选字段需要旧客户端样本测试。

### R02 冻结的 Rust 子集

[R02 设计](../../../docs/eos/design/runtime/grpc-gateway.md)、[首次使用重放](../../../docs/eos/develop/grpc-gateway.md)和[机器用例](../../../tests/fixtures/r02_grpc_gateway/README_cn.md)限定本切片。权威 wire 文件仍是 `share/eos/contracts/eos/` 中的 `gateway.proto`、`capabilities.proto`、`robot/{descriptor,primitive}.proto`、`system/resource.proto` 和 `perception/camera.proto`；R02 不改变包、服务、方法、字段号或 ProtoJSON 身份。Rust listener 实现空服务名的标准 gRPC health `Check`、`island.eos.v1.GatewayService` 的 `Discover`、`ListResources`、`GetStatus`、`ReadResource`、`WatchResource`、`CaptureFrames`，以及 `island.eos.v2.CapabilityService` 的 `ListCapabilities`、`SubmitOperation`、`WatchOperation`、`GetOperation`、`CancelOperation`。默认只绑定回环地址；可选的所有者权限 Unix socket 需要单独配置，绝不公开 MuJoCo 私有 socket。其他已发布方法在实现前返回 `UNIMPLEMENTED`；但 Rust 技能目录不可用时，`SkillService.ListSkills` 返回 `UNAVAILABLE`，现有 EGIA 客户端将其视为可选的空技能集。未提供服务名的 health 为 `NOT_SERVING`。

必需方法及消息身份由已发布 proto 固定：

| RPC | 请求 → 响应 | EGIA 使用方式与 R02 结果 |
| --- | --- | --- |
| health `Check` | `HealthCheckRequest` → `HealthCheckResponse` | 空服务名只在完整发现后为 `SERVING`。 |
| `Discover` | `Empty` → `DiscoverResponse` | 解析机器人、设备和内嵌 primitive binding。 |
| `ListResources` | `Empty` → `ListValue` | 解析 `{uri, class}`；空列表有效。 |
| `GetStatus` | `Empty` → `Struct` | 要求匹配的 `robot_id`/`robot_name` 以及非空 `status`/`version`/`started_at`；报告实时健康。 |
| `ReadResource` | `ResourceRequest` → `Struct` | 读取一个已列出、已校验 URI。 |
| `WatchResource` | `ResourceRequest` → stream `ResourceChanged` | URI 通知后再读取。 |
| `CaptureFrames` | `CaptureFramesRequest` → `CaptureFramesResponse` | 校验一或两张不同 JPEG。 |
| `ListCapabilities` | `ListCapabilitiesRequest` → `ListCapabilitiesResponse` | 将精确能力名称与 Discover binding 配对。 |
| `SubmitOperation` | `SubmitOperationRequest` → `Operation` | 拥有一个 Dispatcher 命令 ID。 |
| `WatchOperation` | `OperationRequest` → stream `Operation` | 跟踪当前及终态。 |
| `GetOperation` | `OperationRequest` → `Operation` | 通过 ID 恢复丢失的 Submit/watch 响应。 |
| `CancelOperation` | `OperationRequest` → `Operation` | 只取消指定命令。 |

一个只有相机的最小准入实例可返回 `Discover`：`{"robot":{"id":"demo","name":"demo-0","model":"eos-runtime","state":"idle","health":"ready","estop":false,"description":"EOS graph runtime demo-0"},"devices":[{"id":"front","class":"camera","part":"front","state":"idle","health":"ready","description":"Front camera"}]}`；`ListResources` 为 `[]`；`GetStatus` 为 `{"robot_id":"demo","robot_name":"demo-0","version":"0.2.0","started_at":"2026-11-01T00:00:00Z","status":"ready"}`；`ListCapabilities` 为 `{"capabilities":[]}`。这些是没有虚构执行器或语义资源的完整发现 ProtoJSON 示例。`CaptureFrames` 请求使用 `{"robotName":"demo-0","deviceId":"front","frameCount":1}`；冷相机返回 gRPC `UNAVAILABLE`，不返回空 `vision.snapshot`。

只有 profile、精确图修订、节点注册表、直接子进程及 adapter、持久 Dispatcher journal 和完整发现投影全部获准，聚合 health 才转为 `SERVING`。当前启动遇到不完整准入会在 listener 绑定前失败且不发 ready 事件；将来若先绑定，必须回答 `NOT_SERVING`，必需 RPC 必须返回 `UNAVAILABLE`。端口已绑定、进程存活或模拟器 socket 可连接都不足以证明就绪。`Discover`、`ListResources`、`GetStatus` 和 `ListCapabilities` 从同一不可变已准入代际投影。R02 不在进程内热替换已 Apply 图：无效候选在发布前被拒绝，现有 listener 保留最后一个完整代际；实时健康可独立降级。图提供机器人 `id` 和 `name`；图尚无物理型号元数据时，`model` 固定为 `eos-runtime`，`description` 说明它是 EOS 图运行时，不宣称硬件型号。`GetStatus` 包含与 Discover 相等的 `robot_id`、`robot_name`、非空 `status` 和 `version`（`0.2.0`），以及 RFC 3339 `started_at`。EGIA 在重新发现时比较 `robot_name`；缺失该字段是无效投影。设备 ID、类别、part、说明和 primitive binding 仅来自已准入 profile 节点及已验证类型元数据。没有可执行绑定的设备仍以真实健康状态列出。能力目录必须与发现结果的机器人/设备 primitive 精确对应；不能指向缺失或不同的 binding。设备暂时不健康可使能力不可用，但身份不变。

`ListResources` 返回按 URI 排序的有界 `{uri, class}` `google.protobuf.ListValue`；URI 以 `robots/{robot.name}/` 开始，仅当生产者及 schema 获准才列出。不得为了满足 EGIA 虚构空的 world、detection 或 ASR 结果。`ReadResource(ResourceRequest.uri)` 返回已列出 URI 的最新已校验 `google.protobuf.Struct`；未知 URI 返回 `NOT_FOUND`，已声明但没有健康新鲜产品返回 `UNAVAILABLE`。硬件采样超过一秒视为过期。`WatchResource` 接受同一精确 URI，在保留产品变化后发送有界且仅含 URI 的 `ResourceChanged`；通知本身不是产品。重连后重建 watch 并重新读取，服务端没有游标或无损历史重放。慢消费者以 `RESOURCE_EXHAUSTED` 断开。所选 EGIA 客户端当前只对 `NOT_FOUND` 或 `UNAVAILABLE` 自动重新发现；对 `RESOURCE_EXHAUSTED` 则把错误交给调用方。R02 不宣称 EGIA 会自动从该状态恢复；调用方必须显式重建 stream 并重新读取，或者另行评审 EGIA 客户端重试变更。取消会释放订阅。

`CaptureFrames` 要求精确的已准入 `robot_name`、已发现且健康的 camera `device_id`，以及等于 1 或 2 的 `frame_count`。它返回 `type=vision.snapshot`、`schema_version=1.0`、同一设备 ID 和精确数量的**互不重复**完整 JPEG 帧，尺寸非零，采集时间是非空 RFC 3339。采集使用 EOS 自有有界相机缓存与截止时间；响应最多 16 MiB，每幅 JPEG 最多 8 MiB，每台相机最多保留两帧。无效数量或名称返回 `INVALID_ARGUMENT`/`NOT_FOUND`；缺帧、过期、不健康或不同帧不足返回 `UNAVAILABLE`；容量耗尽返回 `RESOURCE_EXHAUSTED`。不得启动无限图像流。

能力名称使用 [CapabilityService](capabilities_cn.md) 规定的规范 hardware/runtime 形式。`SubmitOperation` 要求一个目录名称、显式存在的对象型 `arguments`，以及调用方提供的 1–128 字节 `operation_id`；R02 将现有 journal 的 120 字节上限扩展至公开上限。任何 adapter release 之前校验精确 binding、primitive schema、数值限制、健康和截止时间；与其他 EOS 入口共用 Dispatcher 预留及物理释放路径。R03 才增加图修订/权限策略与更广的执行器准入测试；R02 不宣称这些关卡已通过。journal 持久保存足以在重启后重建规范 `primitive.ack` 的不可变 primitive/version/target/准入元数据，不创建第二个 operation store。相同 ID 和相同规范请求在重启前后返回保留状态；相同 ID 的冲突请求返回 `ALREADY_EXISTS`，绝不再次释放。不支持转换的 primitive 保持不可用，不得绕过 Dispatcher 公告。`Operation.operation_id` 始终等于命令 ID。Pending 没有 acknowledgement；终态显式设置 `accepted`，ack 中的 `command_id`、primitive、version 和 status 必须匹配。现有运动 Dispatcher `Completed`、`Cancelled`、`Rejected`、`InProgress`、`OutcomeUnknown` 分别映射为 `SUCCEEDED`、`CANCELLED`、`REJECTED`、`PENDING`、`OUTCOME_UNKNOWN`。R02 stop 命令在终态停止故障已确认时还产生 `FAILED`；R03 才增加更广的运动故障和超时原因。准入前拒绝为 `accepted=false`；准入后故障为 `accepted=true`。已释放但结果不确定的运动重启后仍为 `OUTCOME_UNKNOWN`，不得自动重试。

Stop 操作使用独立的 Dispatcher 请求种类，并遵循同一持久 Prepared → Releasing → Terminal 阶段和 operation ID 规则；它们**不是**普通 `Movement` 值或图控制边。journal 保存有类型的硬件节点或机器人目标，不以虚构硬件路径表示全机器人停止。重启后不确定的 estop 保留安全 latch，但不再次物理释放。`motion.stop` 要求一个已准入设备 ID，只调用该设备的可停止 adapter，在终态停止确认后成功；设备原已停止时成功且无额外物理影响。`robot.stop` 的设备 ID 为空，在一次逻辑释放前按规范路径排序并持久快照最多 64 个已准入、可停止的执行器。独立的持久运动 inhibit 从 Releasing 前直到已验证完成或恢复一直阻止新运动，对快照中每个部件最多调用一次，等待各终态边界，并在 `acknowledgement.result.parts` 报告最多 64 项逐部件结果，每个脱敏原因最多 256 字节；任一部件未确认时，根据故障是否已知，整体为 `FAILED` 或 `OUTCOME_UNKNOWN`。它不接合 emergency-stop latch。`safety.estop` 的设备 ID 也为空，持久记录请求后接合已配置 emergency-stop 路径和进程 latch；成功要求终态确认，不提供 reset。任何 stop 在释放后丢失响应或崩溃都属不确定，不会用同一 ID 重放。`robot.stop` 部分扇出时崩溃返回 `OUTCOME_UNKNOWN`：只有已持久确认的部件结果仍为已知，其余为 unknown；该 ID 下不会自动再次调用任何部件。超过 64 个可停止部件时网关不得就绪；释放前逐部件结果溢出为 `RESOURCE_EXHAUSTED`，可能已释放后的溢出保持 `OUTCOME_UNKNOWN`。现有未 journal 化的 `stop_node`、`emergency_stop` 不能直接充当公开 operation，必须包入独立 journal 化 Dispatcher 路线并实现终态确认语义。停机时仍可独立调用 emergency stop 保证安全。无法确认终态停止的 adapter 不能返回 `SUCCEEDED`。

RobotStop 的运动 inhibit 是 journal 控制状态，区别于 `safety.estop` latch。它在 Releasing 前同步落盘，所有 Dispatcher 运动释放（包括并发请求）都要检查。全部终态停止确认后，`robot.stop` 才持久清除该状态；已知部分失败、不确定结果、崩溃或清除落盘失败均保持 inhibit。部分扇出崩溃后，同一 ID 的 `OUTCOME_UNKNOWN` 只可查询。运动被 inhibit 时，可用**新的、按本地策略授权的** operation ID 提交 `robot.stop`；其目标快照必须覆盖尚未解决的旧目标与当前全部已准入可停止执行器，并在每个目标的终态停止边界得到确认后才持久清除。任一旧目标无法验证已停时，运动保持 inhibit，等待具有等价逐目标停止证据的显式授权维护；不得自动清除或重放。inhibit 期间，完整 gRPC 发现接口可保持 `SERVING`，使停止恢复路线可达；但 `GetStatus.status` 与机器人 health 为 `degraded`，普通运动能力 `available=false`，Submit 运动在释放前拒绝。Stop 能力保持可用。正常 ready 事件只证明运行时安全响应，不授予运动权限；客户端必须读取实时健康与能力可用性。清除该 inhibit 不会清除紧急停止 latch。

Dispatcher 释放协调器封闭持久 `Releasing` 记录与物理 adapter 调用之间的间隙。运动在协调器下取得释放许可，检查 inhibit 和截止时间，持久写入 `Releasing`，并在物理调用期间一直持有许可；取得许可就是释放的线性化点，已写入 `Releasing` 但在 adapter 前暂停的运动仍持有该许可。RobotStop 先关闭新的运动许可，再于自身 `Releasing` 前同步写入 inhibit，并等待先前运动许可全部结束，或 adapter 证明其物理效果已被隔离。`motion.stop` 对其目标设备采用同样的顺序。物理结果模糊或延迟的运动在 adapter 证明静止且无待执行命令，或在物理写入边界拒绝旧释放代次前，仍属未解决；仅从调用返回或观察一次已停样本都不足够。若在有界停止截止时间内无法得到证明，RobotStop 保持 `OUTCOME_UNKNOWN` 和 inhibit，不宣称成功，需使用新的授权 ID 或等价维护恢复。协调器等待 adapter 时不持有 journal 互斥锁；独立的 latch 型 `safety.estop` 路线可抢先处理阻塞的运动。进程重启后，旧的 `Releasing` 运动在 adapter 代次隔离或等价物理静止且无待执行命令得到验证前，须封锁新的运动释放；期间健康状态降级、运动不可用。在此证明与全部目标停止确认前不得清除 RobotStop inhibit。这个顺序阻止先前在 adapter 调用前暂停的运动于已确认 RobotStop 后再执行。

`GetOperation` 返回当前保留状态；`WatchOperation` 先发送该状态，若 pending 则至多再发送一个终态，没有游标。watch 断开后，客户端先以同一 ID 调用 `GetOperation`，再决定下一步。`CancelOperation` 仅通过 Dispatcher 取消该命令；终态 ID 返回保留结果，未知 ID 返回 `NOT_FOUND`，不可取消命令返回 `FAILED_PRECONDITION`。运动钳制需要显式 stop 能力。请求取消或丢失响应不等于物理取消。关闭时聚合 health 转为 `NOT_SERVING`，新提交返回 `UNAVAILABLE`，watch 关闭，Dispatcher stop 先于模拟器终止及回收（R06 提供退出证据）。

格式错误或空 ID、缺失对象参数、非法枚举/帧数以及超大请求返回 `INVALID_ARGUMENT`；未知已准入名称、URI、相机 ID 或 operation ID 返回 `NOT_FOUND`；命令身份冲突返回 `ALREADY_EXISTS`；有界队列、stream 或帧容量耗尽返回 `RESOURCE_EXHAUSTED`；不可用或过期依赖返回 `UNAVAILABLE`；传输截止时间过期返回 `DEADLINE_EXCEEDED`。已准入语义拒绝是 OK RPC 加终态 `REJECTED`，不是传输错误。`INTERNAL` 仅表示服务不变量或释放前持久存储失败；若可能已经释放，则保留 `OUTCOME_UNKNOWN`。历史非受管理查询对缺少规范 ack 元数据的旧 journal ID 返回 `DATA_LOSS`，不重放；受管理查询遵循下文统一的 `NOT_FOUND` 策略。普通 unary 请求最多 64 KiB，帧响应最多 16 MiB，每种方法最多 16 个订阅，工作受调用方截止时间约束；缺失截止时间时默认五秒。传输 `x-request-id` 仅用于关联，不能代替 operation 身份。

## R03 执行器受治理准入目标契约

**R03 经合并后的 EOS 与 IsOS 所有者验收。** [R03 设计](../../../docs/eos/design/runtime/admission-revision.md)、[首次使用重放](../../../docs/eos/develop/admission-revision.md)与[冻结用例](../../../tests/fixtures/r03_admission_revision/README_cn.md)限定受管理 EOS 准入切片。所选 `aether-runtime --capabilities` 探针声明精确特性 `managed-control-grants`。对 `0.1.0` 受管理图，IsOS P05 必须要求所选 runtime 提供该令牌；旧 runtime 在停止活动子进程或签发授权前，于其进程 `/node_type` 拒绝。只增加元数据与私有启动输入，不改变已发布 protobuf 包、方法、字段号或 ProtoJSON 名称。IsOS 拥有保存整图 SHA-256 `active_revision`；EOS `projection_sha256` 只是生成 EOS 投影字节摘要，不能替代它。所选 IsOS schema-2 叶目录为 `data/generated/<deployment>/v2/<revision>/<generation>/`，清单在叶目录的 `manifest.json`，`eos/graph.json` 位于其下。IsOS 启动前验证该叶目录。EOS 要求 `manifest.schema_version:2`，所选 profile 恰为清单列出的普通 `eos/graph.json`，拒绝符号链接和路径逃逸，核对刚读取字节与唯一清单条目的摘要、64 位小写 `manifest.revision`，并在进程期间固定两种身份。受管理模式同时要求 `EOS_CONTROL_GRANTS_FILE` 和符合 IsOS `/v2/<revision>/<generation>/eos/graph.json` 布局的所选 profile 路径；仅有其中一项时，在 listener 绑定、ready 事件或 adapter 构造前启动失败。两项都有时，schema-2 清单或验证文件缺失、损坏、不匹配也使启动失败，且 EOS 有效 `--run-id` 必须等于验证文件的 `run_id`。可选的公开 `--run-id` 单独只用于关联，不能选择受管理模式。两项都没有时，独立 profile 可提供完整只读发现，运动能力 `available=false`；即使提供 `--run-id` 也不宣称保存图活动修订，Submit 返回 `UNAVAILABLE`。IsOS 仍可不启动此受管理执行器路径而安全读取并显式迁移旧 `1.0.0` 保存图。

IsOS 每次部署运行创建两个由 OS 密码学随机源产生的独立 32 字节授权：`egia-control` 和 `operator-control`。私有 mode-0700 runtime 目录下的每个 mode-0600 普通授权文件恰含 64 位小写十六进制数字及一个换行；解码后的 32 字节是秘密。另一个 mode-0600 JSON 验证文件包含 `schema_version:1`、精确 `run_id`、保存图 `active_revision`，以及解码授权的 SHA-256 摘要 `egia_control_sha256`、`operator_control_sha256`；该文件不含秘密。IsOS 只以 `EOS_CONTROL_GRANTS_FILE` 将验证文件路径交给 EOS，只以 `EGIA_EOS_CONTROL_GRANT_FILE` 将 EGIA 授权路径交给受监管 EGIA，只以 `AETHER_BRIDGE_CONTROL_GRANT_FILE` 将操作员授权路径交给受监管 HTTP bridge。EOS 将运行和修订与已准入代际比较，再以常量时间比较所提交授权的摘要与相应角色验证值。所有权、权限、路径或内容不安全时写入就绪失败。图字节、生成配置、日志、描述符和公开元数据绝不存授权。IsOS 签发与启动交付正在独立的所有者分支实现；跨仓库重放通过前，EOS 夹具注入授权只能证明 EOS 行为。信任边界是本机 OS 用户，不宣称隔离同 UID 的另一进程。授权寿命仅覆盖一个 IsOS 部署 `run_id`：同一次运行内 EOS 子进程重启复用按运行固定的验证文件和授权以恢复 journal；新部署运行签发新授权并在写入就绪前使旧文件失效。授权字节绝不进入不可变生成快照。即使保存图修订相同，EOS 仍拒绝旧验证文件的 `run_id`。

对于 gRPC 执行器写入及操作查询、watch 和取消，必须提供 `authorization: Bearer <64 位小写十六进制授权>`。运动、`motion.stop` 和 `robot.stop` 必须提供 `x-eos-active-revision: <64 位小写十六进制>`，并在排队后 Dispatcher 最终释放闸门再次比较。完整发现响应增加 `eos-active-revision` 与 `eos-projection-sha256` 元数据；客户端不需修改 protobuf 响应消息即可将后续写入绑定到已检查的代际。旧只读客户端可继续无新元数据的 health、发现、资源与能力列举；缺少授权或修订的写入在物理释放前失败。两个授权都可在修订缺失或过期时经保留通道调用 `safety.estop`。内部关闭使用独立私有安全接口。持久运动 inhibit 期间，只有 `operator-control` 在全新 ID 的 `robot.stop` 上明确提供 `x-eos-stop-recovery: true` 才能设置 `StopRequest.authorized_recovery`；EOS 快照旧未解决目标与当前已准入可停止执行器的并集，所有目标终态证明已停前保持 inhibit。`egia-control` 或没有标记的操作员请求不能恢复 inhibit。`robot.stop` 针对整机，`motion.stop` 只针对一个精确已准入设备。恢复和受控停止都不清除紧急 latch。`egia-control` 仅可经受治理 gRPC 提交运动。`operator-control` 仅可经私有 `primitive_typed` bridge 路径提交运动；其 gRPC 运动 Submit 返回 `PERMISSION_DENIED`。两个角色可按下表使用已认证 gRPC stop。EOS 不会仅凭授权推断 EGIA 安全阶段已通过。私有 `primitive_typed` header 增加 `active_revision`；受监管 bridge 在外层 owner-socket 请求中发送 `authority_grant`，浏览器载荷不含此值。A02 为 EGIA 的受管理写入增加元数据。已合并的 EOS bridge 为 `primitive_typed` 注入私有操作员授权；R07 仍负责 UI 修订展示、按权限校验的结果后续查询及完整浏览器路径验收。

| 角色与入口 | 运动 | 受控停止 | `safety.estop` | 操作 Get/Watch/Cancel |
| --- | --- | --- | --- | --- |
| `egia-control` gRPC | 精确活动 binding | 精确设备 `motion.stop`、正常 `robot.stop`；无 inhibit 恢复 | 可用，修订过期/缺失仍可 | 仅自身持久角色/范围及本次运行 |
| `operator-control` gRPC | 拒绝 | 精确设备 `motion.stop`、正常 `robot.stop` 及显式全新 ID inhibit 恢复 | 可用，修订过期/缺失仍可 | 仅自身持久角色/范围及本次运行 |
| `operator-control` 私有 bridge IPC | 精确活动 `primitive_typed` binding | R07 bridge 契约评审后才转发受治理停止 | 受治理所有者路径 | 后续 R07 结果查询执行相同持久角色/范围校验 |
| 无有效授权或 `egia-control` 私有 IPC | 拒绝 | 拒绝 | 拒绝 | 拒绝 |

| 字段 | 类型与单位 | 用途、缺省、校验与兼容 |
| --- | --- | --- |
| `authorization` | gRPC ASCII 元数据，— | Bearer 加 64 位小写授权；缺失、格式错误或未知时预留前返回 `UNAUTHENTICATED`。有效授权但角色/范围错误返回 `PERMISSION_DENIED`。不记录或回显。 |
| `x-eos-active-revision` | gRPC ASCII 元数据，保存字节 SHA-256 | 普通执行器写入必填；缺失/格式错误为 `INVALID_ARGUMENT`，格式正确但过期为 `FAILED_PRECONDITION`。已认证 `safety.estop` 可省略。 |
| `eos-active-revision` | 响应元数据，保存字节 SHA-256 | 只在已验证受管理代际存在；独立/旧只读路径缺失。 |
| `eos-projection-sha256` | 响应元数据，投影字节 SHA-256 | EOS 投影诊断身份；不能满足 `x-eos-active-revision`。 |
| `x-eos-stop-recovery` | gRPC ASCII 元数据，布尔字面量 | 仅 `operator-control` 在全新 `robot.stop` ID 上提供 `true` 可授权 inhibit 恢复；缺省为 false，格式错误或其他角色使用在释放前返回 `PERMISSION_DENIED`。 |
| `header.active_revision` | 私有 `primitive_typed` 字符串，保存字节 SHA-256 | bridge 运动必填，在最终释放处比较；旧 header 不能驱动执行器。 |
| `authority_grant` | 私有 owner-socket 字符串，— | 由受监管 bridge 而非浏览器请求提供；同样的 64 位格式与角色校验；不进入日志或 journal。 |

例如，受管理 `SubmitOperation` 保留既有 ProtoJSON body `{"capabilityName":"robots/demo-0/devices/drive/primitives/base.velocity","operationId":"cmd-17","arguments":{"x_m_s":0.1,"y_m_s":0,"yaw_rad_s":0,"valid_for_ms":100}}`，另附上述两项请求元数据。缺少授权返回 gRPC `UNAUTHENTICATED`，没有 `Operation` 且 adapter 调用数为零。已认证但修订过期返回 `FAILED_PRECONDITION`，不新增预留或 adapter 调用。`GetOperation`、`WatchOperation` 和 `CancelOperation` 要求有效授权的角色/范围与该 ID 持久保存的提交者一致；查询旧结果无需修订仍活动。对受管理 listener，未授权角色、错误运行、封存的旧 ID 及不存在的 ID，在 Get、Watch、Cancel 均统一返回 `NOT_FOUND`，不泄露结果或改变取消状态。历史非受管理的进程内查询对缺少准入身份的旧记录仍返回 `DATA_LOSS`；它不能授权受管理写入。角色是所述同 UID 边界内的所有者单位，授权轮换后仍适用。已授权 `GetOperation` 可恢复丢失响应且绝不调用 adapter；新命令不能使用旧修订。同一 IsOS 部署运行内 EOS 子进程重启保留运行 ID 与授权，可继续按精确 ID 恢复丢失响应。显式新 IsOS 部署运行轮换授权和运行 ID；旧授权失败，新授权查询/watch/取消旧运行 ID 返回与不存在的 ID 相同的 `NOT_FOUND`，复用旧命令 ID 返回 `ALREADY_EXISTS`，不把先前成功当作本次动作，也不释放新命令。同一 ID 更换角色、修订、binding、目标或类型化参数返回 `ALREADY_EXISTS`，绝不再次释放。持久签名包含稳定角色/范围、服务端证明的入口、IsOS `run_id` 与保存修订，不含凭据字节、传输 `x-request-id` 或单次尝试截止时间；停止记录还持久保存 `MotionStop`/`RobotStop`/`SafetyEstop` 种类、精确设备或机器人目标、角色、运行 ID、保存修订（仅 estop 可为空）和恢复标记。同 ID 跨角色或改变恢复标记的停止产生冲突，不能重放此前结果；每次新尝试在释放前仍须有有效期限。

所有部署用的公开及私有执行器写入汇入 Dispatcher 受治理准入；运动和停止使用不同的协调方法及保留停止通道。当前 owner-socket `manual`、`primitive`、`skill_execute`、`skill_result`、原始 `dispatch`、未 journal 化的 `stop` 与 `manual_stop`，以及原始 `cancel`、`result`、`skill_cancel` 不得继续作为生产执行器或结果路径。`GetOperation` 及 R07 私有结果后续查询必须执行相同的持久角色/范围校验；`CancelOperation` 是唯一公开取消路径。生产中的 `skill_cancel` 不得调用未 journal 化的 `stop_node`。所有者专用内部关闭安全接口保持可用。R03 取消 HTTP bridge 转发 `shutdown`。私有 runtime Unix socket 保留仅所有者可调用的 `shutdown`，由 mode-0600 socket、mode-0700 父目录和与 EOS 所有者比较的 peer UID 校验保护；非所有者在任何停止副作用前失败。IsOS Supervisor 是预期调用方。R06 单独验证 EOS 执行器停止先于模拟器子进程终止/回收；仅向进程组发送 SIGTERM 不能证明此顺序；本地进程内 helper 和历史图类型不能授予部署权限。adapter release 前，Dispatcher 校验精确已准入 binding/目标、角色、活动修订、健康的生命周期及健康状态、新鲜类型化观测、有限数值限制、单调期限、持久 ID 和安全状态。已准入语义拒绝仍是关联命令 ID 的终态 `REJECTED`；释放前持久化失败时不调用 adapter；可能释放但终态落盘丢失时成为 `OUTCOME_UNKNOWN`，该 ID 不再重试。R01 证据队列上限不能作用于命令或固定安全路径。R02 stop 类型和保留容量保持独立。MuJoCo 物理代次隔离与模拟器停止/回收顺序仍属 R05/R06。

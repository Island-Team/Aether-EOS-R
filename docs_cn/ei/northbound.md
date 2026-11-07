# Northbound WebSocket API

[English](../../docs/ei/northbound.md)

EGIA 在 `/api/aether/egia/` 下提供 JSON WebSocket route。Listener 来自 `etc/egia/egia.toml` 的 `[server]`，默认地址为 `127.0.0.1:16060`。Browser Origin 仅允许 `https://www.os-island.com`；没有 Origin header 的本地 operator client 可连接。可选 sandbox media route `/v1/sandbox` 与此 API 隔离。

## 路由

| Route | 形式 | 用途 |
|---|---|---|
| `/health` | stream | EGIA runtime、subsystem 与 safety health |
| `/backend` | stream | EOS 连接及紧凑 robot/device 状态 |
| `/chat` | command | operator hearing ingress |
| `/missions/stream` | stream | mission table projection |
| `/missions/command` | command | 创建或修改 pending operator mission |
| `/review` | command | review 或提升 mission history |
| `/context` | stream | 最新的有界动态 model context |
| `/model_io` | stream | model request、activity 与 reply event |
| `/prompts` | stream | 当前静态 planner prompt |
| `/primitives` | stream | ready 的类型化 EOS primitive binding |
| `/skills` | stream | v2 native skill catalog 和逐机器人 availability |
| `/skills/command` | command | 精确 skill 查询、启动、execution 查询和取消 |
| `/skills/execution` | watch | 从指定 cursor 之后开始的有序 execution snapshot |
| `/primitive/invoke` | command | 经过 safety gate 的 operator primitive 调用 |
| `/perceptions` | stream | 已准入 observation history |
| `/perception/world/state` | stream | 融合 `world.snapshot@1` |
| `/perception/world/detection` | stream | 语义 `detection.snapshot@1` |
| `/perception/world/asr` | stream | 紧凑 `asr.snapshot@2` |

当前 API 没有 scene、vision、pose、VAD、body-status、raster-map 或 Agent-local execution route。未知及已移除 path 会直接关闭，不提供 redirect 或 alias。Camera JPEG 通过 southbound 按需 `CaptureFrames` 获取，而不是 northbound image stream。

## Stream 行为

Snapshot route 在连接时发送当前值，并在 store version 变化时发送替换值。Source 未 ready 时发送 `{"error":"unavailable"}`，随后以 WebSocket code 1013 关闭。World snapshot 保留 EOS sequence、timestamp、类型化 identity 与 source provenance；fleet 不会静默合并两个 robot 的状态。

## Backend 状态

`/backend` 为每个已配置的稳定 robot alias 返回一个 entry。连接 bookkeeping 由 EGIA 管理；body identity 与 state 来自当前 EOS 原子代次：

```json
{
  "backends": [
    {
      "backend": "robot-a",
      "connection": "up",
      "endpoint": "127.0.0.1:16003",
      "eos_robot_id": "01a...",
      "robot_name": "example-robot",
      "eos_started_at": "2026-09-23T00:00:00Z",
      "eos_version": "3.0.0",
      "robot_model": "example-model",
      "robot_description": "mobile manipulator",
      "robot_state": "idle",
      "robot_health": "ready",
      "estop": false,
      "power_pct": 82.0,
      "devices": [
        {
          "device_id": "gripper-0",
          "device_class": "gripper",
          "part_id": "left",
          "description": "left gripper",
          "state": "idle",
          "health": "ready",
          "primitives": ["gripper.motion"]
        }
      ],
      "last_heartbeat_at": "2026-09-23T00:00:02Z",
      "consecutive_failures": 0
    }
  ]
}
```

由 `eos_started_at` 而不是稳定 robot ID 标识 EOS 重启。只有列出的紧凑 body field 才是公开字段；driver/provider detail 与 raw telemetry 始终属于 EOS 私有信息。

## Primitive catalog

`/primitives` 将每个 ready 可执行 binding 连同 portable schema 与部署 metadata 一起投影：

```json
{
  "primitives": [
    {
      "name": "gripper.motion",
      "description": "Release one open or close motion",
      "device_description": "left workcell gripper",
      "version": "1.0.0",
      "robot_id": "robot-a",
      "device_id": "gripper-0",
      "eos_robot_id": "01a...",
      "operator": false,
      "scope": "device",
      "binding_kind": "direct",
      "health": "ready",
      "references": [],
      "absolute_frame": null,
      "dependencies": [],
      "accepted_media_types": [],
      "limits": {
        "force_n": {"minimum": 0.0, "maximum": 10.0}
      },
      "arguments_schema": {"type": "object"}
    }
  ],
  "count": 1
}
```

`version` 是精确 SemVer。`robot_id` 是稳定的配置 alias，`eos_robot_id` 是诊断 provenance，只有 robot-scoped binding 的 `device_id` 为 null。Registry 在出口前拒绝未知 version、不可用 binding、无效 target、schema violation 和超出范围的 numeric argument。

## Native skill

`/skills` 发布 schema version 2，并报告 availability 及通过校验的 skill metadata：配置的 robot alias、EOS identity、精确 name/version、description、parameter/result schema、精确 primitive reference 和 resource template。EOS service 临时不可用会用 degraded snapshot 替换陈旧 claim；格式错误的 descriptor 会拒绝 EOS 代次。

`/skills/command` 每帧接受一个严格 tagged request，未知字段会被拒绝。`get_skill` 要求 `robot_id`、`name` 和 `version`；`start` 还要求调用方生成的非空 `request_id`、object-valued `arguments`，以及大于零且不超过 3600 的 `timeout_s`；`get_execution` 和 `cancel` 要求 `robot_id` 与 `execution_id`。成功响应包含 `descriptor` 或完整的 `execution`。稳定错误包括 `invalid_json`、`invalid_request`、`robot_not_found`、`skill_not_found`、`execution_not_found`、`request_conflict`、`capacity_exhausted`、`unavailable`、`timeout` 和 `internal_failure`。

```json
{"operation":"start","robot_id":"robot-a","name":"approach","version":"1.2.0","request_id":"ui-42","arguments":{"distance_m":1.5},"timeout_s":30}
```

`/skills/execution` 只接受一个初始 `{"robot_id":"robot-a","execution_id":"...","after_sequence":4}` request。它转发所有 sequence 更大的 retained complete snapshot，继续跟随 live transition，并在 EOS 关闭 terminal watch 后结束。重连时携带最后处理的 sequence 可避免重复。断开 WebSocket 不会取消 execution；取消必须通过 `/skills/command` 显式请求。

## Operator primitive 调用

向 `/primitive/invoke` 发送一个精确 target：

```json
{
  "primitive": "gripper.motion",
  "robot_id": "robot-a",
  "device_id": "gripper-0",
  "arguments": {"action": "open"},
  "correlation_id": "ui-42"
}
```

调用方不得在 `arguments` 中放入 `command_id`。EGIA 生成 command identity，在执行路径持久化，并单独保留 client `correlation_id`。Robot-scoped `robot.stop` 与 `safety.estop` 省略 `device_id`；其他 primitive 必须提供该字段。

Backend terminal answer 是显式的：

```json
{
  "ok": true,
  "accepted": true,
  "command_id": "2aa1d76e-85b7-4fd0-8aef-8a4136b76555",
  "status": "succeeded",
  "sequence": 4,
  "result": {"status": "succeeded"},
  "reason": null,
  "reason_code": null,
  "correlation_id": "ui-42"
}
```

只有 `status == "succeeded"` 表示成功。`rejected`、`failed`、`cancelled`、`timed_out` 和 `outcome_unknown` 都是 terminal non-success state。顶层 `reason` 和 `reason_code` 只在 backend admission rejection 时存在；已准入后的失败细节保留在 `result` 内。Local request、target、policy、timeout 或 egress failure 使用 `ok: false`、`error` 和可选 `reason`。

Operator ingress 与 planner action 使用相同 registry、policy、safety gate、command lifecycle 和 EOS egress。`[primitives].expose_operator = false` 会在该 route 隐藏 operator-tier primitive；启用后也不会让它们对 planner 可见。

## Hearing、mission 与 perception

`/chat` 接受 `{"sentence":"..."}` 并追加带来源的 hearing observation；它不会直接创建 mission。Mission stream 暴露当前 document、priority、cancellation 和 terminal action outcome。三个 world stream 暴露可替换的类型化 snapshot，不包含 image、audio、prompt、ASR text 之外的 transcript 或 provider payload。

## Mission command

`/missions/command` 接受严格的 `create`、`update`、`priority` 和 `cancel` operation。Server 管理 mission UUID、source、origin、planning 与 execution state。Mutation 要求精确 mission UUID 和当前 `expected_revision`；持久化失败会在发布前报告。稳定 error 包括 `invalid_json`、`invalid_request`、`queue_full`、`mission_not_found`、`mission_revision_conflict`、`mission_not_pending`、`mission_protected`、`mission_started`、`mission_mutation_failed`、`storage_failure` 和 `internal_failure`。

## Schema 所有权

EGIA-owned northbound schema 位于已安装 package 的 `egia/contracts/schemas/northbound/`，并由 `scripts/generate_northbound_schemas.py` 从 Pydantic wire model 生成。EOS protobuf 和共享 action/resource schema 仍由 EOS 管理。已移除 route 与 wire shape 没有 compatibility alias。

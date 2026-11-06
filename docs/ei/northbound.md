# Northbound WebSocket API

[中文](../../docs_cn/ei/northbound.md)

EGIA serves JSON WebSocket routes under `/api/aether/egia/`. The listener comes from `[server]` in `etc/egia/egia.toml` and defaults to `127.0.0.1:16060`. Browser Origin is restricted to `https://www.os-island.com`; local operator clients without an Origin header are accepted. The optional sandbox media route `/v1/sandbox` is isolated from this API.

## Routes

| Route | Form | Purpose |
|---|---|---|
| `/health` | stream | EGIA runtime, subsystem, and safety health |
| `/backend` | stream | EOS connection plus compact robot/device state |
| `/chat` | command | operator hearing ingress |
| `/missions/stream` | stream | mission table projection |
| `/missions/command` | command | create or mutate pending operator missions |
| `/review` | command | review or promote mission history |
| `/context` | stream | latest bounded dynamic model context |
| `/model_io` | stream | model request, activity, and reply events |
| `/prompts` | stream | current static planner prompt |
| `/primitives` | stream | ready typed EOS primitive bindings |
| `/skills` | stream | v2 native-skill catalog and per-robot availability |
| `/skills/command` | command | exact skill lookup, start, execution lookup, and cancellation |
| `/skills/execution` | watch | ordered execution snapshots after a supplied cursor |
| `/primitive/invoke` | command | safety-gated operator primitive invocation |
| `/perceptions` | stream | admitted observation history |
| `/perception/world/state` | stream | fused `world.snapshot@1` |
| `/perception/world/detection` | stream | semantic `detection.snapshot@1` |
| `/perception/world/asr` | stream | compact `asr.snapshot@2` |

The API has no scene, vision, pose, VAD, body-status, raster-map, or Agent-local execution route. Unknown and retired paths close without redirects or aliases. Camera JPEGs are available southbound through on-demand `CaptureFrames`, not through a northbound image stream.

## Stream behavior

Snapshot routes send the current value on connection and then replacements when the store version changes. A source that is not ready sends `{"error":"unavailable"}` and closes with WebSocket code 1013. World snapshots retain their EOS sequence, timestamp, typed identity, and source provenance; a fleet never silently merges two robots' state.

## Backend status

`/backend` returns one entry per configured stable robot alias. Connection bookkeeping is EGIA-owned; body identity and state come from the current atomic EOS generation:

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

`eos_started_at`, not the stable robot ID, identifies an EOS restart. Only the listed compact body fields are public; driver/provider detail and raw telemetry remain private to EOS.

## Primitive catalog

`/primitives` projects each ready executable binding with its portable schema and deployment metadata:

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

`version` is exact SemVer. `robot_id` is the stable configured alias, `eos_robot_id` is diagnostic provenance, and `device_id` is null only for robot-scoped bindings. The registry rejects unknown versions, unavailable bindings, invalid targets, schema violations, and out-of-range numeric arguments before egress.

## Native skills

`/skills` publishes schema version 2 and reports availability plus validated skill metadata: configured robot alias, EOS identity, exact name/version, description, parameter/result schemas, exact primitive references, and resource templates. Transient EOS service unavailability replaces stale claims with a degraded snapshot; malformed descriptors reject the EOS generation.

`/skills/command` accepts one strict tagged request per frame. Unknown fields are rejected. `get_skill` requires `robot_id`, `name`, and `version`. `start` additionally requires a non-empty caller-generated `request_id`, object-valued `arguments`, and `timeout_s` greater than zero and no greater than 3600. `get_execution` and `cancel` require `robot_id` and `execution_id`. Successful replies contain either `descriptor` or a complete `execution`; stable failures are `invalid_json`, `invalid_request`, `robot_not_found`, `skill_not_found`, `execution_not_found`, `request_conflict`, `capacity_exhausted`, `unavailable`, `timeout`, and `internal_failure`.

```json
{"operation":"start","robot_id":"robot-a","name":"approach","version":"1.2.0","request_id":"ui-42","arguments":{"distance_m":1.5},"timeout_s":30}
```

`/skills/execution` accepts exactly one initial `{"robot_id":"robot-a","execution_id":"...","after_sequence":4}` request. It proxies every retained complete snapshot with a greater sequence, follows live transitions, and closes after EOS closes the terminal watch. Reconnect with the last processed sequence to avoid duplicates. Disconnecting the WebSocket does not cancel execution; cancellation is explicit through `/skills/command`.

## Operator primitive invocation

Send one exact target to `/primitive/invoke`:

```json
{
  "primitive": "gripper.motion",
  "robot_id": "robot-a",
  "device_id": "gripper-0",
  "arguments": {"action": "open"},
  "correlation_id": "ui-42"
}
```

The caller may not put `command_id` in `arguments`. EGIA mints the command identity, persists it on the execution path, and preserves the separate client `correlation_id`. Robot-scoped `robot.stop` and `safety.estop` omit `device_id`; other primitives require it.

A backend terminal answer is explicit:

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

Only `status == "succeeded"` is success. `rejected`, `failed`, `cancelled`, `timed_out`, and `outcome_unknown` are terminal non-success states. Top-level `reason` and `reason_code` are populated only for backend admission rejection; admitted failure details remain inside `result`. Local request, target, policy, timeout, or egress failures use `ok: false` with `error` and optional `reason`.

Operator ingress uses the same registry, policy, safety gate, command lifecycle, and EOS egress as planner actions. `[primitives].expose_operator = false` hides operator-tier primitives from this route; enabling it never makes them planner-visible.

## Hearing, missions, and perception

`/chat` accepts `{"sentence":"..."}` and appends an attributed hearing observation; it does not directly create a mission. Mission streams expose current documents, priorities, cancellations, and terminal action outcomes. The three world streams expose replaceable typed snapshots without images, audio, prompts, transcripts beyond ASR text, or provider payloads.

## Mission commands

`/missions/command` accepts strict `create`, `update`, `priority`, and `cancel` operations. The server owns mission UUID, source, origin, planning, and execution state. Mutations require the exact mission UUID and current `expected_revision`; persistence failure is reported before publication. Stable errors include `invalid_json`, `invalid_request`, `queue_full`, `mission_not_found`, `mission_revision_conflict`, `mission_not_pending`, `mission_protected`, `mission_started`, `mission_mutation_failed`, `storage_failure`, and `internal_failure`.

## Schema ownership

EGIA-owned northbound schemas live under `egia/contracts/schemas/northbound/` in the installed package and are generated from the Pydantic wire models with `scripts/generate_northbound_schemas.py`. EOS protobuf and shared action/resource schemas remain EOS-owned. Removed routes and wire shapes have no compatibility aliases.

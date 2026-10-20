# north HTTP and WebUI contract

[North HTTP implementation](../../ext/north_http/README.md) · [Native gRPC interfaces](../README.md) · [Configuration](../../ext/north_http/config.md) · [中文](../../../../../docs_cn/eos/develop/interfaces/external/http_cn.md)

`eos-north-http` is the only browser-facing northbound process. It translates REST, Server-Sent Events (SSE), and the live monitor WebSocket to the canonical Gateway, Skill, and Workflow gRPC services exposed by `eosd`. EOS itself listens only for gRPC. `eos-repl` uses this same HTTP/SSE boundary, so its integration tests are executable client fixtures for an out-of-tree WebUI.

The process constructs one shared gRPC `Client` for the configured `upstream`. Its one channel backs one generated Gateway stub, one Skill stub, and one Workflow stub; all REST calls, SSE streams, and readiness probes reuse that set. The HTTP `Server` borrows the client and owns request metadata, deadlines, active-call cancellation, health checks, routing, and wire conversion. See the [module map](../../ext/north_http/README.md#module-map).

## Contents

- [Start, health, and configuration](#start-health-and-configuration)
- [REST routes](#rest-routes)
- [SSE streams](#sse-streams)
- [Audit routes](#audit-routes)
- [Errors, CORS, and request IDs](#errors-cors-and-request-ids)
- [Operations and migration](#operations-and-migration)

## Start, health, and configuration

Start EOS and north HTTP as independent processes:

```sh
./bin/eosd --config "$PWD/etc/eos/eos.example.toml"
./bin/eos-north-http --config "$PWD/etc/eos/north-http.toml"
```

The default listener is `127.0.0.1:16005`, with EOS at `127.0.0.1:16003`. North HTTP may start while EOS is absent. In that state `GET /health/live` remains `200`, `GET /health/ready` and dependent requests return `503`, and later calls reconnect through the gRPC channel automatically. ActionList audit health is reported independently at `/v1/eos/audit/status`; audit degradation never changes process liveness, functional readiness, or EOS core health.

`etc/eos/north-http.toml` is strict-keyed:

| Key | Default | Meaning |
| --- | --- | --- |
| `host`, `port` | `127.0.0.1`, `16005` | HTTP/SSE listener |
| `upstream` | `127.0.0.1:16003` | EOS gRPC target |
| `allow_remote` | `false` | required opt-in for a non-loopback listener |
| `allowed_origins` | Island WebUI and local port 3000 origins | exact CORS allowlist |
| `max_request_bytes` | `67108864` | maximum HTTP request body |
| `max_connections` | `64` | total accepted-connection capacity |
| `max_inflight_requests` | `32` | unary-request capacity |
| `max_sse_streams` | `16` | SSE stream capacity |
| `max_monitor_streams` | `4` | monitor WebSocket capacity |
| `connect_timeout_ms` | `2000` | readiness gRPC deadline |
| `request_timeout_ms` | `630000` | unary gRPC deadline; 630000 ms maximum |
| `sse_heartbeat_ms` | `15000` | SSE heartbeat interval |

The four connection-capacity values must be positive and are capped at 256 each. The unary, SSE, and monitor capacities must sum to no more than `max_connections`; the parser checks this without overflowing.

The strict `[action_list_audit]` table defaults to `enabled = false`, `rotation_size_bytes = 16777216`, `rotation_files = 8`, and `reconnect_delay_ms = 1000`; the numeric values must be positive and are capped at 268,435,456 bytes, 64 files, and 60,000 ms, with an overflow-safe size/count product cap of 256 MiB. The committed reference configuration explicitly enables audit. Storage is always derived as `${state}/north-http/action-list-audit`; there is no user path key.

The strict parser and defaults belong to the [config module](../../ext/north_http/config.md); runtime enforcement belongs to the [server module](../../ext/north_http/server.md). Changing `upstream` changes the single process client target rather than creating per-route channel configuration.

Unknown keys, wrong types, non-positive limits, and a non-loopback `host` without `allow_remote = true` fail before the listener opens. `allow_remote` does not add authentication or encryption. Production deployments must keep the north HTTP on loopback or terminate TLS, authentication, authorization, rate limits, and public CORS policy at a trusted reverse proxy.

The executable accepts `--config`, `--etc`, `--contracts`, `--host`, `--port`, `--upstream`, and `--log-level`; endpoint/log overrides take precedence over the selected file. Source users run `bin/eos-north-http`. Installed systems use `/usr/bin/eos-north-http`, native code under `/usr/libexec/island`, and the default `/etc/island/eos/north-http.toml`. The wrapper is independent of the caller's working directory.

## REST routes

Requests and responses use protobuf JSON names. Request bodies are strict ProtoJSON: malformed JSON and unknown fields return `400` instead of being ignored. Path fields are authoritative; a conflicting skill reference, execution ID, robot name, or navigation-point name in a body is rejected.

| Method | Path | gRPC operation |
| --- | --- | --- |
| `GET` | `/health/live` | Process liveness; no gRPC call |
| `GET` | `/health/ready` | Upstream gRPC readiness |
| `GET` | `/v1/eos/discover` | `GatewayService.Discover` |
| `GET` | `/v1/eos/status` | `GatewayService.GetStatus` |
| `GET` | `/v1/eos/resources` | `GatewayService.ListResources` |
| `GET` | `/v1/eos/resource?uri=...` | `GatewayService.ReadResource` |
| `GET` | `/v1/eos/robots/{robot_name}/navigation/waypoints` | `GatewayService.ListNavigationPoints` |
| `GET` | `/v1/eos/robots/{robot_name}/navigation/waypoints/{name}` | `GatewayService.GetNavigationPoint` |
| `PUT` | `/v1/eos/robots/{robot_name}/navigation/waypoints/{name}` | `GatewayService.PutNavigationPoint` |
| `DELETE` | `/v1/eos/robots/{robot_name}/navigation/waypoints/{name}` | `GatewayService.DeleteNavigationPoint` |
| `POST` | `/v1/eos/navigation/waypoints:reload` | `GatewayService.ReloadNavigationPoints` |
| `POST` | `/v1/eos/resource:history` | `GatewayService.QueryResourceHistory` |
| `POST` | `/v1/eos/primitives:call` | `GatewayService.CallPrimitive` |
| `POST` | `/v1/eos/motions:cancel` | `GatewayService.CancelPrimitive` |
| `POST` | `/v1/eos/world/camera:capture` | `GatewayService.CaptureFrames` |
| `POST` | `/v1/eos/perception/objects:observe` | `GatewayService.ObserveObjects` |
| `POST` | `/v1/eos/perception/vad:infer` | `GatewayService.InferVad` |
| `POST` | `/v1/eos/perception/humans:detect` | `GatewayService.DetectHumans` |
| `POST` | `/v1/eos/perception/human-poses:estimate` | `GatewayService.EstimateHumanPose` |
| `POST` | `/v1/eos/perception/faces:detect` | `GatewayService.DetectFaces` |
| `GET` | `/v1/eos/perception/roles` | `GatewayService.GetPerceptionRoles` |
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
| `GET` | `/v1/eos/audit/status` | Local external-audit health and retained cursor |
| `GET` | `/v1/eos/audit/action-lists?limit=N` | Local retained audit records |

Normalized base, arm, and gripper status require no dedicated HTTP routes. They are listed by `/v1/eos/resources`, read through `/v1/eos/resource?uri=...`, and watched through the existing resource SSE endpoint using the URIs `robots/{robot}/base/status`, `robots/{robot}/arm/status`, and `robots/{robot}/gripper/status`.

The four perception routes are unary, typed one-shot inference calls. Each body selects one exact `modelId` and carries media as ProtoJSON base64 in `input.data`; optional thresholds, aligned depth media, and tracked persons are typed fields. EOS rejects a model with the wrong capability and never falls back to another configured model. Face detection exposes recognition only; face registration remains a primitive.

Object observation is a separate on-demand camera-and-model operation. Send `POST /v1/eos/perception/objects:observe` with a configured robot, camera, and enabled detection-capability model:

```json
{"robotName":"robot-0","deviceId":"cam-wrist","modelId":"litter-detector","sourceTimestampNs":"1791436400123456789"}
```

The response has `deviceId`, `modelId`, `frameId`, `sourceTimestampNs` (a decimal nanosecond string in ProtoJSON), `widthPx`, `heightPx`, `jpeg` (base64 JPEG), and `objects`. Each object retains the normalized model's `label`, `confidence`, pixel `bbox` (`[x1,y1,x2,y2]`), optional pixel `polygon` (`[[x,y],...]`), and optional `class_id`/`track_id` (object entries retain the model's JSON names). EOS copies one camera sample before calling the model, so the returned JPEG and detections refer to the same image. This is a general observation, not a grasp or motion plan. For a deployment camera bridge, `sourceTimestampNs` is the Unix-nanosecond stamp shared with its paired observation stream. The request field is optional: omit it to observe the latest camera sample, or provide the stamp of an already-received ROS cloud to infer **that exact frame**. Do not pair by nearest arrival time. An exact-stamp request requires telemetry history; if its frame has been evicted or is over two seconds old, EOS returns `503` rather than silently using a different image.

The camera must be configured, alive, and provide a nonempty frame ID, positive source timestamp, dimensions, and a valid JPEG within its configured image-byte limit (4 MiB by default). A telemetry-hub frame older than two seconds is rejected. A nonpositive requested stamp returns `400`. An unknown robot, camera, or model returns `404`; a disabled or wrong-capability model returns `412`; an unhealthy, absent, stale, or invalid camera sample returns `503`. Model timeouts/failures use the ordinary gRPC-to-HTTP error mapping below. No camera device is opened by this route.

Role discovery returns only the deployment's `vad`, `human`, `pose`, and `face` bindings. Each entry contains `role`, `modelId`, current `available`, and whether the role is `continuous`; `cameraDeviceId` identifies the camera used by the configured continuous human pipeline. Clients use these bindings for the unary calls and must not ask operators to select arbitrary provider model IDs. Because ProtoJSON omits default scalars, `continuous` is absent when false; clients normalize an absent value to `false` rather than rejecting the role.

## Live monitor WebSocket

`GET /v1/eos/monitor/ws` upgrades to a read-only WebSocket backed by `GatewayService.WatchMonitor`. A browser sends exactly one text subscription:

```json
{"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","include_detections":true}
```

`version`, `type`, and `camera_device_id` are required. `microphone_device_id`, `include_detections`, and `detection_source` are optional; unknown fields are rejected. North HTTP requests a maximum camera rate of 30 FPS; EOS validates the selected devices. A malformed subscription receives `monitor.error` and close code `1008`. An origin header not in `allowed_origins` is rejected with HTTP `403` before upgrade. Subscription validation happens before version negotiation, so its error envelope uses version `1`; clients diagnose it from `code` and `message`, not from that envelope version.

Ready, status, audio, and error events are text JSON with `version: 1` and respective types `monitor.ready`, `monitor.status`, `monitor.audio`, and `monitor.error`. Ready contains the selected device IDs plus `width_px`, `height_px`, `fps`, `sample_rate_hz`, and `channels`, plus `detection_status` (`available`, `not_configured`, or `unavailable`). The microphone ID is omitted when absent. Status contains `state` and `message`. Audio contains `rms`, `peak`, `dbfs`, `clipping`, `sequence`, `captured_at`, `sample_rate_hz`, and `channels`, plus `source_timestamp_ns` when supplied by EOS. All 64-bit values are decimal strings. No raw PCM crosses this interface.

Camera events are binary. The first 12 bytes are ASCII `EOSM`, version byte `1`, kind byte `1` (JPEG), two reserved zero bytes, and a big-endian unsigned 32-bit metadata length. That many UTF-8 JSON bytes follow, then the JPEG bytes. Metadata contains `sequence`, `captured_at`, `width_px`, `height_px`, and `dropped_frames`, plus optional `source_timestamp_ns`. All 64-bit metadata values are decimal strings. Clients must reject unknown versions/kinds, nonzero reserved bytes, impossible lengths, invalid metadata, and empty image payloads.

The WebSocket propagates `X-Request-ID` to gRPC and returns it in the upgrade response. Closing the browser cancels the upstream stream. EOS owns latest-frame backpressure; north HTTP writes each received event directly and never logs JSON, JPEG, or audio content. `monitor.error` reports an upstream failure before the server closes when the transport still permits it.

Version 1 admits camera-only subscriptions and configured live detections. Set `detection_source` to `"object"` to stream litter detections from the selected camera; omitted or `"human"` preserves human detections from the configured human-perception camera. Raw JPEGs remain binary kind `1`; the envelope version is `1`. Binary kind `2` carries metadata containing `sequence`, `generation`, optional `source_timestamp_ns`, dimensions, and the complete canonical `detection.snapshot`, followed by the exact retained source JPEG used for that snapshot. Object snapshots include model labels and bounding boxes in `detections`. Pose and face enrichment may publish a later kind-2 frame for the same generation, replacing annotations without pixel mismatch. Kind-2 human frames are emitted only for the configured human-perception camera. Kind-2 object frames are available for any selected admitted camera.

The protobuf annotations are the versioned route source. Render the complete message and route inventory with `./scripts/print_contract_interfaces.py`.

### JSON wire rules

The frontend does not need generated protobuf code, but it must follow ProtoJSON exactly:

- field names are lower camel case (`workflowId`, `afterSequence`); fields inside schema-owned `Struct` values keep their documented spelling;
- `uint64` values such as `sequence`, `eventSequence`, and `revision` are JSON decimal strings. Keep them as strings or `BigInt`, never a JavaScript number;
- timestamps are RFC 3339 strings, durations look like `"30s"`, and protobuf `bytes` are standard padded base64 strings;
- enums use full symbolic names such as `"EXECUTION_STATUS_RUNNING"`;
- absent/default scalar fields may be omitted. Normalize missing repeated fields to `[]` and omitted scalar defaults to their protobuf value (`false`, `0`, or `""` as appropriate), but do not confuse absence with explicit JSON `null`. A zero `sequence` or `monitorSequence` may therefore be absent;
- unknown fields, wrong types, and malformed enum/duration/timestamp values are rejected. Send `Content-Type: application/json` for POST and PUT requests.

Path parameters must be URL encoded. A path value is authoritative: repeating that field in a POST body is valid only when identical; a conflict returns `400`. `GET /v1/eos/resource` requires one non-empty URL-encoded `uri` query.

### Gateway payloads

`GET /v1/eos/discover` returns the complete device/primitive discovery snapshot. `GET /v1/eos/status` returns aggregate runtime and robot status. Its `models` object retains the aggregate `status`, `total`, `ready`, `checking`, `degraded`, and `unavailable` fields and also includes `items` for every configured model. Each item reports `id`, `capability`, normalized `health` (`ready`, `degraded`, or `unavailable`), and the runtime diagnostic `message`; disabled models are listed as unavailable with `model disabled`. `GET /v1/eos/resources` returns resource descriptors, and `GET /v1/eos/resource?uri=...` returns one typed resource envelope. Their fields and schema meanings are specified in [body](../body.md) and [world](../world.md); HTTP uses the JSON mapping above. Device resources normally include `class`. Schema-owned retained resources may instead include `schema_uri` and `history` without `class`; `uri` is the only field common to every descriptor.

Every primitive binding includes `parameters`, an object JSON Schema for its `arguments`. For example, a binding may return `{"parameters":{"type":"object","properties":{"posture":{"type":"string","enum":["stand","walk","lie"]}},"required":["posture"],"additionalProperties":false}}`. Use this value to build controls and validation for that exact device. Keep `limits`, `references`, and `absoluteFrame` as separate compatibility and operating-envelope fields. Older EOS versions may omit `parameters`; clients must then retain raw JSON entry or another local-contract fallback.

Primitive call body:

```json
{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"ui-command-018f","arguments":{"linear_x_m_s":0.1,"linear_y_m_s":0,"angular_z_rad_s":0,"valid_for_ms":250}}
```

The response is the acknowledgement in [primitives](../actions.md). Cancel with `{"commandId":"ui-command-018f"}`. Frame capture returns JPEG `bytes` as base64; a browser may display a bounded result with a `data:image/jpeg;base64,...` URL.

### Skills and executions

`GET /v1/eos/skills` returns `{"skills":[SkillDescriptor...]}`. Each descriptor contains its `reference`, description, parameter/return JSON Schemas, primitive/resource dependencies, signal descriptors, claim templates, and `reentrant`. Build forms from the schemas instead of hard-coding arguments.

Start body (name and version come from the path):

```json
{"arguments":{"robot_name":"robot-0","gimbal_device_id":"gimbal-0"},"requestId":"ui-observe-42"}
```

Omitting `timeout` runs without an execution deadline. Supply a positive duration such as `"timeout":"30s"` to retain bounded execution behavior.

Start, get, terminate, and execution-watch responses use this shape:

```json
{"executionId":"exec-...","requestId":"ui-observe-42","reference":{"name":"observe_human","version":"1.0.0"},"status":"EXECUTION_STATUS_RUNNING","sequence":"2","cancellationRequested":false,"reasonCode":"","message":"","queueReason":"","claims":[],"activeDeviceClaims":[],"latestJournalSequence":"3","runtimeId":"builtin"}
```

`SUCCEEDED`, `REJECTED`, `CANCELLED`, `TIMED_OUT`, and `FAILED` are terminal. `result` is meaningful only when emitted by the skill; do not invent `{}` when it is absent. Show `queueReason` while queued and `reasonCode`/`message` on failure. Cancellation is a request, so keep observing until terminal.

Signal body and receipt:

```json
{"requestId":"ui-signal-7","name":"confirm","payload":{"accepted":true}}
```

```json
{"signalId":"sig-...","executionId":"exec-...","requestId":"ui-signal-7","accepted":true}
```

An execution journal entry contains `journalSequence`, `timestamp`, and exactly one of `snapshot`, `signal`, or `record`. A record identifies its system/skill source and activity/checkpoint/suppression kind. Diagnostic logs remain operator-only and never appear in this stream. Branch on oneof field presence. See [SkillService](../skills.md) for claims, replay, signals, and lifecycle semantics.

### Workflows and runtime snapshot

```json
{
  "definition": {
    "workflowId": "greet-visitor-42",
    "actions": [
      {"actionId":"observe","skill":{"name":"observe_human","version":"1.0.0"},"parameters":{"robot_name":"robot-0","gimbal_device_id":"gimbal-0"}},
      {"actionId":"greet","skill":{"name":"speak","version":"1.0.0"},"parameters":{"text":"Hello"},"dependsOn":["observe"],"when":{"factKey":"human.present","equals":true,"maxAge":"5s"}}
    ]
  }
}
```

The returned `ActionList` contains string `revision`, symbolic list/item statuses, timestamps, and complete item snapshots. Skill items link through `executionId`; primitive items use `commandId`. Replace cached state only with a newer revision. `GET /v1/eos/workflows` returns `{"actionLists":[...]}`; use the Workflow SSE stream below for live revisions and fact events.

`GET /v1/eos/runtime:snapshot` returns `capturedAt`, `executions`, `facts`, `actionLists`, and `actionListCursor`. ActionLists, facts, and that cursor are one atomic view; skill executions retain their independent sequencing. Bootstrap the workflow view from this snapshot, then watch strictly after its cursor. See the complete [WorkflowService contract](../workflow.md). An idle fresh runtime may serialize the cursor as `{"epoch":"..."}` because its zero sequence is omitted; normalize that missing sequence to `"0"`.

```sh
curl -sS http://127.0.0.1:16005/v1/eos/discover

curl -sS -H 'Content-Type: application/json' \
  -H 'X-Request-ID: operator-smoke-1' \
  -d '{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"018f-example","arguments":{"linear_x_m_s":0.1,"linear_y_m_s":0,"angular_z_rad_s":0,"valid_for_ms":250}}' \
  http://127.0.0.1:16005/v1/eos/primitives:call

curl -sS -H 'Content-Type: application/json' \
  -d '{"arguments":{"robot_name":"robot-0","gimbal_device_id":"gimbal-0"},"requestId":"observe-human-1","timeout":"45s"}' \
  http://127.0.0.1:16005/v1/eos/skills/observe_human/versions/1.0.0/executions
```

## SSE streams

| Path | Event name | `id` | Resume input |
| --- | --- | --- | --- |
| `/v1/eos/resource:watch?uri=...` | `resource-changed` | none | none |
| `/v1/eos/executions/{id}:watch?afterSequence=N` | `skill-execution` | execution `sequence` | non-empty `Last-Event-ID`, then query |
| `/v1/eos/executions/{id}/state:watch?afterRevision=N` | `skill-execution-state` | state `revision` | non-empty `Last-Event-ID`, then query |
| `/v1/eos/executions/{id}/journal:follow?afterJournalSequence=N` | `skill-journal-entry` | `journalSequence` | non-empty `Last-Event-ID`, then query |
| `/v1/eos/executions:monitor?skillName=NAME&skillVersion=VERSION&status=QUEUED,RUNNING&afterMonitorSequence=N` | `skill-monitor-update` | `monitorSequence` | non-empty `Last-Event-ID`, then query |
| `/v1/eos/workflows:watch?cursor=epoch:sequence` | `action-list-event` | `epoch:sequence` | non-empty `Last-Event-ID`, then query |

Each `data:` line is one ProtoJSON message. Heartbeats are SSE comments (`: heartbeat`) and carry no state. A non-empty `Last-Event-ID` header is authoritative on reconnect; the query cursor is used when that header is absent or empty. Clients should persist the last fully processed ID, reconnect with it, and tolerate replay according to the underlying gRPC stream contract. SSE gRPC contexts intentionally have no unary request deadline; they remain open until the upstream finishes, the HTTP peer disconnects, or the server shuts down.

Execution state is the skill-published public payload, separate from lifecycle snapshots and the operator journal. `GET .../state` returns its current `executionId`, monotonic `revision`, `publishedAt`, and `payload`; the state watch emits only revisions after the requested cursor. Resource history is queried by POSTing exactly one sequence or timestamp/range selector to `/v1/eos/resource:history`; responses include retained products plus retention bounds, and do not turn the live resource watch into a replay stream.

`GET /v1/eos/executions` is the one-shot HTTP interface behind the REPL `top` command; the REPL filters and renders that retained execution snapshot locally. Without a status filter, `top` includes terminal records so immediate failures remain visible. The execution SSE monitor is the HTTP interface behind `htop`. With no selector it reports active `QUEUED` and `RUNNING` skills. `skillVersion` requires `skillName`; `status` accepts a comma-separated list of execution status names. Each update contains complete upserted execution snapshots and removed execution IDs, so clients can maintain a live process table. The initial empty reset may be only `{"reset":true}`: omitted `monitorSequence`, `upserted`, and `removedExecutionIds` mean `"0"`, `[]`, and `[]` respectively. A Workflow cursor contains an opaque non-empty epoch and unsigned sequence. Persist its SSE `id` only after applying the complete ProtoJSON `ActionListEvent`; deduplicate records at or before the durable cursor and require the next same-epoch sequence. `FAILED_PRECONDITION` covers an epoch mismatch or future cursor, while `OUT_OF_RANGE` means the cursor predates retained events; either result requires a fresh runtime snapshot followed by a new watch from `actionListCursor`. A missing or malformed cursor is `INVALID_ARGUMENT`.

A dedicated peer-disconnect monitor promptly cancels the upstream gRPC context, including while a stream is otherwise idle; a failed HTTP write is a second cancellation path. Slow or disconnected clients cannot create an unbounded north HTTP queue because the service writes directly under the bounded request and stream lifetime.

Browser reconnect pattern:

```js
let cursor = localStorage.getItem("skill-journal-cursor") ?? "0";
function watch(executionId) {
  const id = encodeURIComponent(executionId);
  const source = new EventSource(
    `/v1/eos/executions/${id}/journal:follow?afterJournalSequence=${encodeURIComponent(cursor)}`
  );
  source.addEventListener("skill-journal-entry", event => {
    renderJournalEntry(JSON.parse(event.data));
    cursor = event.lastEventId;
    localStorage.setItem("skill-journal-cursor", cursor);
  });
  source.addEventListener("error", event => {
    if (event.data) renderApiError(JSON.parse(event.data));
    source.close();
    scheduleReconnect(() => watch(executionId));
  });
}
```

Persist a cursor only after rendering succeeds. EventSource reconnects after a network close; explicit backoff prevents a tight loop after typed errors. If a deployment requires custom authentication headers, use `fetch()` and parse the SSE stream because EventSource cannot set arbitrary headers. Abort that fetch with `AbortController` when leaving the screen; disconnect cancellation is propagated to the upstream gRPC stream.

After an SSE response has started, stream failures are delivered as a typed `event: error` whose `data` is the error JSON below. Unary failures, including an unavailable upstream, use the mapped HTTP status; an SSE connection already has HTTP status `200` and therefore reports the failure in-band. This also applies to a missing or invalid stream query after SSE headers have been committed.

```sh
curl -N -H 'Accept: text/event-stream' \
  'http://127.0.0.1:16005/v1/eos/executions/EXECUTION_ID/journal:follow?afterJournalSequence=0'
```

## Audit routes

The optional ActionList audit is a local, external, non-authoritative projection that consumes the same shared `Client` and `WorkflowService` stream as the WebUI. EOS core never reads this journal, and audit failure cannot mutate or stop core execution. Both audit routes set `Cache-Control: no-store`.

`GET /v1/eos/audit/status` returns `enabled`, `health`, `running`, and, when enabled, stable `error`, `cursor`, and retained-record `records` fields. Health is `stopped`, `healthy`, or `degraded`; errors are `none`, `invalid_config`, `io`, `corrupt_chain`, or `resource_exhausted`. A disabled process returns `{"enabled":false,"health":"disabled","running":false}`.

`GET /v1/eos/audit/action-lists` returns `{enabled, records, truncated}`; disabled audit returns an empty list and `truncated: false`. `limit` defaults to 100 and must be 1–1000. The server measures the encoded records JSON array under a fixed 4 MiB budget, selects one contiguous newest suffix, and returns it chronological oldest-first; `truncated` says older requested records were omitted. If the newest record alone exceeds the budget, it returns HTTP `429` / `RESOURCE_EXHAUSTED` with `newest audit record exceeds response byte limit`. Records contain only the documented workflow/action/execution/command/device/target IDs, enum statuses, timestamps, revision, cursor, chain fields, and narrowly allowed fact timing/source-sequence fields; they never contain arguments, results, reasons, schemas, request bodies, authentication data, raw protobuf, or fact key/value/source/frame/unit. Operational IDs remain sensitive.

The append-only journal lives under the derived state directory with owner-only permissions, rotation, atomic cursor checkpoints, and a SHA-256 chain. That chain provides corruption evidence, not authenticity against an attacker able to rewrite records and hashes. See the [audit module contract](../../ext/north_http/audit.md) for exact storage, recovery, resume, privacy, and degradation behavior.

## Errors, CORS, and request IDs

Every JSON error has the stable envelope:

```json
{"code":14,"message":"EOS gRPC upstream is not ready","requestId":"..."}
```

`code` is the numeric gRPC status. The main HTTP mappings are invalid argument `400`, unauthenticated `401`, permission denied `403`, not found `404`, already exists/aborted `409`, failed precondition `412`, resource exhausted `429`, unimplemented `501`, unavailable `503`, deadline exceeded `504`, and data loss `502`; client cancellation uses `499`, and unexpected failures use `500`. Unknown routes return `404`. Oversized bodies return `413`; exceeding the active-request bound returns `429`.

A client may send one `X-Request-ID` using 1–96 ASCII letters, digits, `.`, `_`, `:`, or `-`. Invalid or duplicate IDs return `400`; an absent ID is generated. North HTTP returns `X-Request-ID`, includes it in errors, propagates it as gRPC metadata, and EOS binds the same ID to unary, stream, and asynchronous skill-worker logs. Logs record route/RPC, status, duration, sizes, readiness, and cancellation—not request bodies, credentials, media, transcripts, or skill payloads.

CORS is exact-origin allowlisting. Allowed origins receive `GET, POST, PUT, DELETE, OPTIONS`, the `Content-Type`, `Last-Event-ID`, and `X-Request-ID` request headers, and an exposed `X-Request-ID` response header. An origin not on the list receives no allow-origin header.

Successful API responses have no application cache contract. Treat them as live state, prevent shared/proxy caching in the deployment, and retain the last confirmed snapshot in UI memory. A transport error must not replace known state with an invented empty object.

## Operations and migration

Logs are isolated under `var/log/eos/runs/<launch-id>/<service>/`, with `events.jsonl` plus routed logs directly in the service directory. Retained launches use unique IDs; overwrite mode reuses `current`, and `var/log/eos/latest` selects the newest run. Correlate one request by searching the same `correlation_id` in the north HTTP and EOS event files.

Root process composition owns process start/stop/failure records, the server owns listener/upstream/request/stream transport records, and the shared client intentionally owns no request logging or context policy. This split does not change the public error envelope or correlation behavior.

Diagnostic events and the durable audit journal have separate ownership. `north-http.audit` diagnostics are routed to `north-http/ext-north-http-audit.log` and the process `events.jsonl`; `north-http.client` has a reserved `north-http/ext-north-http-client.log` route but currently emits no events. `[log]` rotation, overwrite, deduplication, and attachment settings never modify the audit journal. Use the [debugging runbook](../../../usage/debugging.md) to correlate request IDs, Workflow cursors, gRPC failures, and audit recovery without collecting excluded payloads.

The old `[gateway].json_port` and `eosd --json-port` settings are removed, not deprecated. Move the HTTP port and CORS settings to `north-http.toml`, start the north HTTP process separately, and point browser/REPL clients at it. Production service managers should own `eosd`, `eos-north-http`, and any simulator as separate processes; `eosd` never launches north HTTP.

See the [diagnostic REPL guide](../../ext/tools/repl.md) for the executable client and guided parameter workflow.

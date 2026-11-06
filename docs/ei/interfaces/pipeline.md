# EGIA stage interface, target 0.1.0

[中文](../../../docs_cn/ei/interfaces/pipeline.md) · [Architecture](../design/architecture.md) · [Current IsOS adapter](isos-config.md)

## Boundary and status

This is the **target**, not an implemented wire protocol. The stage interface is an in-process, typed Python boundary projected into the IsOS graph for configuration and observation. IsOS owns the serialized graph, source-edge object, versioned node-type catalog, and revision-checked edits; see its [node graph contract](../../isos/interfaces/README.md). EOS owns gRPC and physical payloads. EGIA owns the stage values and algorithms below. The A01 EGIA reader validates the seven exact stage types and a complete one-instance-per-type graph, renders a revision-bound `stages.json`, and binds each instance to its existing Python core owner at managed launch. A04 adds owner-side scheduling for two exact evidence routes. Stub EOS evidence traverses the typed link → perception → context → decision path. Mission/plan state and fixed safety and emergency egress remain with their current owners. IsOS G06 still rejects EGIA edge QoS until its separate route registration; result/health feedback is also outside this A04 slice.

The graph uses `0.1.0` for newly authored documents and `@v0.1` for new type IDs. Each registered type has a nonblank `description` and each configured instance retains its own `description`. Readers accept existing `1.0.0` graph documents and the exact historical `egia.common.source@v1.0`, `egia.common.context@v1.0`, `egia.common.planner@v1.0`, and `egia.common.guard@v1.0` design identities only for their established behavior; no legacy design type acquires an actuator route. New `@v0.1` stages are new identities, never automatic aliases or migrations of those four types. Unsupported future versions or unknown types fail validation with the path and supported set. Adding optional fields and new ports is compatible; changing a port's meaning, units, direction, or existing type ID requires a new type version. Existing protobuf field numbers and EOS service names never change because of this stage catalog.

The managed `egia/stages.json` overlay carries `revision: string`, exactly 64 lowercase hexadecimal SHA-256 digits of IsOS's saved whole graph. It is a selected deployment identity, not an EOS projection digest or an authored graph setting. EGIA rejects a missing or malformed value before Link readiness and requires EOS's `eos-active-revision` to equal it on every complete discovery generation, including forced refresh. A consistent EOS response for a different saved graph never publishes ready stages or admits a primitive.

## Shared stage value

Stage ports pass typed objects in Python. The JSON below is a **debug projection** for a log or inspector, not a new transport that replaces EOS protobuf or EGIA WebSocket contracts:

```json
{
  "schema_version": "0.1.0",
  "event_id": "01JAZQ3D5Q4N8PK1Y3V6W0M2BS",
  "run_id": "run-20261101-01",
  "source_node": "egia/evidence/perception/main",
  "backend": "robot-a",
  "observed_at": "2026-11-01T08:00:00Z",
  "kind": "observation",
  "payload": {"resource_ref": "camera/front", "status": "available"}
}
```

| Field | Type / unit | Purpose and default | Validation, owner, compatibility |
|---|---|---|---|
| `schema_version` | semantic version string / none | Debug projection schema; required. | EGIA; exact supported `0.1.0`, new optional fields may be added, unknown major/minor rejected by a reader that cannot safely interpret them. |
| `event_id` | opaque nonblank string / none | Identifies one event; required. | EGIA producer; unique within a run, preserved across retries of the same event. |
| `run_id` | opaque nonblank string / none | Correlates process launch; required. | EGIA supervisor; inherited by all stages, never re-used for a new launch. |
| `source_node` | canonical `core/layer/group/node` path / none | Unambiguous attribution; required. | EGIA projection; must resolve to a configured node of the declared type; a leaf ID alone is invalid. |
| `backend` | stable backend alias or null / none | Identifies source EOS; defaults null for operator-only events. | EGIA link; non-null alias must exist in rendered mode profile. Never substitute an EOS process UUID. |
| `observed_at` | RFC 3339 UTC timestamp / seconds | Source observation time; required for observations, otherwise event creation time. | EGIA source stage; finite, parseable, retain original time and record receipt time separately for staleness checks. |
| `kind` | enum / none | One of `discovery`, `observation`, `context`, `mission`, `proposal`, `admission`, `rejection`, `result`, `health`. | EGIA; unknown kind rejected, never treated as a generic command. |
| `payload` | kind-specific typed value / none | Stage data; required. | EGIA owning stage; validates shape before forwarding. New optional fields may be ignored by compatible readers; unknown action variants fail closed. |

All graph-configurable stage settings use IsOS setting descriptors with name, type, label, description, default, required flag, validation, and optional selector metadata. `description` is required and nonblank for a new registered type; old sidecars receive a display-only compatibility description at read time. Graph source-edge `priority` and `frequency_hz` are IsOS-owned delivery controls; they do not change mission priority or safety admission.

## Stage catalog and ports

Each type's registered `core` is `egia`. Port names are stable within `@v0.1`; `?` marks optional input. A port is an in-process typed stream or snapshot, not a network listener. `expected_rate_hz` is a producer hint; per-edge `frequency_hz` is a delivery cap in IsOS's graph contract. A nonblank type description must explain the stage's purpose and must be shown with the instance description in IsosUI.

The owner catalog projects each registered port as `data_type: egia.stage.<kind>@v0.1`, `transport: in_process`, and an exact `data_class`. Inputs also declare `cardinality: one` and `required`; process-owned ingress and fixed command egress carry `fixed: true` and cannot be authored as graph edges. `catalog.data_types` contains one object JSON Schema per port kind, with required typed fields and unit annotations where a field has a fixed unit. IsOS may aggregate these `stage_types` into its graph catalog, but EGIA remains the only owner of the schemas. A schema describes the catalog shape; the stage owner still validates live values before forwarding.

The A01 local runtime rejects an event from another run or with an `observed_at` more than 300 seconds old at receipt; it also rejects future or invalid timestamps. This first bound applies to live stage delivery and precedes duplicate tracking. The resource admission boundary requires nonblank `sample.text` and `sample.eos_robot_id` before retaining or forwarding a resource event. Rejections carry a stable code, source node, and event ID; a corrected event can reuse an ID that failed validation.

The A04 owner scheduler compiles edge QoS only for the exact local routes `egia.link.gateway@v0.1.resource → egia.perception.admission@v0.1.resource` and `egia.perception.admission@v0.1.observations → egia.context.builder@v0.1.observations`. An optional `priority` is an integer in `-20..19`, defaults to `0`, and maps to fair-delivery weight `20 - priority`; lower nice values get more service without starving another ready edge. Optional `frequency_hz` is finite and strictly positive; absence leaves the edge uncapped. Each route holds one pending latest value. Replacing it increments public `gaps` and `overflow` counters and records `evidence_overflow`; delivery uses a monotonic clock, and shutdown clears pending evidence. The counters appear in `stage_edge_delivery` owner logs. These controls do not apply to `context.snapshot → decision.snapshot` or to discovery, result, health, registry, mission state, proposal, latch, admission, and emergency-stop routes. Those routes retain their uncapped or fixed owner behavior. IsOS G06 still rejects QoS on EGIA-local edges until its separate exact-route catalog change; this owner implementation alone does not make an IsOS graph active.

| Type ID / layer / group | Input ports | Output ports | Type description and lifecycle |
|---|---|---|---|
| `egia.link.gateway@v0.1` / `io` / `link` | `gateway` (verified EOS endpoint), `admitted` (governed request, internal), `emergency_stop` (monitor-only, internal) | `discovery` (complete generation), `resource` (EOS evidence), `result` (operation result), `health` | Connects one stable backend alias to EOS. Starts before dependent stages; degraded discovery blocks action, shutdown closes the channel. |
| `egia.perception.admission@v0.1` / `evidence` / `perception` | `resource`, `hearing?`, `result` | `observations` (bounded event/snapshot view) | Validates and bounds attributed evidence. Starts after link; invalid evidence is diagnosed and dropped, not transformed into an empty success. |
| `egia.context.builder@v0.1` / `cognition` / `context` | `observations`, `body?`, `memory?` | `snapshot` (bounded model context) | Renders deterministic context without changing source state. Missing optional inputs render absent sections. |
| `egia.missions.queue@v0.1` / `cognition` / `missions` | `intent`, `result?` | `selected`, `lifecycle` | Admits and schedules versioned missions. Persists checkpoints; an empty queue waits. |
| `egia.decision.planner@v0.1` / `cognition` / `decision` | `selected`, `snapshot` | `proposal`, `plan_state` | Uses configured model lanes to propose typed steps. Model failure or malformed output yields a failed proposal, never an action. |
| `egia.safety.gate@v0.1` / `governance` / `safety` | `proposal`, `registry`, `latch` | `admission`, `rejection` | Admits only exact typed primitives. Its command egress is a fixed runtime route to link, inaccessible to arbitrary graph edges. |
| `egia.monitor.health@v0.1` / `governance` / `monitor` | `health`, `progress`, `bounds?` | `status`, `latch` | Watches health and progress. Its emergency egress is a fixed runtime route only. |

| Value or setting | Type / unit | Purpose and default | Validation, owner, compatibility |
|---|---|---|---|
| `gateway` | endpoint reference / none | IsOS-resolved EOS gRPC TCP/UDS endpoint; required for managed mode. | IsOS resolves; EGIA validates transport and absolute UDS path. HTTP ports are invalid. Existing TCP profiles without `transport` remain TCP. |
| `resource` | EOS typed resource reference + sample / source units | Evidence; no default. | EOS owns payload; link validates source and schema. Unknown references do not become capabilities. |
| `discovery` | complete EOS resource/capability generation / none | Link readiness and exact bindings; absent until a full generation completes. | Link owns atomic publication; partial or conflicting discovery fails closed. EOS owns underlying protobuf fields. |
| `hearing` | attributed operator utterance / text and UTC time | Optional evidence; absent when no input source is configured. | EGIA northbound owns identity and size checks; perception rejects malformed or unattributed input. |
| `body` | validated EOS body snapshot / source units | Optional hardware context; absent until discovery. | EOS owns physical descriptors; link verifies robot and resource identity before context reads it. |
| `memory` | bounded recalled records / text and score | Optional prior context; absent when disabled or unavailable. | EGIA memory owns retention and provenance; context caps count and tokens, never treats recall as source truth. |
| `observations` | bounded sequence/snapshot / source units | Evidence available to context; empty when valid but none seen. | Perception owns admission, ordering, coalescing, and omission count. New observation variants require admission policy. |
| `snapshot` | bounded context document / tokens | Model-readable state; empty optional sections allowed. | Context owns total token budget and deterministic ordering; does not contain raw secret or unbounded media. |
| `intent` / `selected` | mission document and revision / none | Operator request and selected work; absent when queue empty. | Missions owns identity, source, priority, version, and checkpoint; stale revision rejected. |
| `lifecycle` | mission ID, revision and state transition / none | Communicates mission changes; absent until a transition occurs. | Missions owns monotonic revisions and durable checkpoint; duplicate transition IDs are idempotent, conflicts fail. |
| `proposal` | typed planned primitive reference / none | Candidate action; no default. | Decision owns model parsing; must reference backend, device, primitive, version and arguments. It has no execution authority. |
| `plan_state` | plan ID, revision, phase and outcome / none | Reports planning progress; absent until a mission is selected. | Decision owns; invalid transitions and stale revision fail, and model timeout is not success. |
| `registry` | complete validated capability generation / none | Safety lookup; absent until discovery succeeds. | Link/registry own exact backend/device/primitive/version bindings; safety rejects missing or stale generations. |
| `admission` / `rejection` | gate decision with reason / none | Release or explain denial; one terminal decision per proposal. | Safety owns; admitted data uses exact current registry binding. Unknown/stale/invalid data fail closed. |
| `result` | EOS operation identity and terminal state / none | Feedback; no default. | EOS owns terminal operation semantics, link validates identity; timeout/cancel is not success. |
| `health` / `latch` | status enum and monotonic episode / none | Report degradation and emergency state; healthy/unlatched only after verified initialization. | Monitor owns; latched stop persists until explicit authorized reset, not process restart. |
| `progress` | mission/operation ID and monotonic elapsed time / seconds | Monitor input; absent when no work runs. | Decision/link own measurements; monitor bounds staleness and never infers completion from lack of updates. |
| `bounds` | finite measured value and configured limit / source unit | Optional monitor safety input; absent when no bound source is configured. | Monitor checks both unit and source identity; missing required limits fail closed. |
| `status` | health state, episode and reason / none | Operator-visible monitor projection; initialized as unavailable. | Monitor owns transitions; stale or partial backend state never maps to healthy. |
| `admitted` / `emergency_stop` | typed command reference / EOS units | Fixed runtime egress, never a user-editable graph edge; no default. | Safety owns workload admission and monitor alone owns emergency stop; link checks caller identity, EOS owns final admission. |
| `model_binding` | nonblank selector ID / none | Selects an installed/configured model endpoint for decision lanes; required where lane enabled. | EGIA validates binding exists in `models.toml`; selector options come from IsOS package/config catalog. No raw weight path in graph. Adding a binding does not admit a new model action kind. |
| `backend_alias` | unique string / none | Stable route to EOS; required on a link instance. | EGIA validates uniqueness and nonblank value; process UUID is diagnostic only. |

The `@v0.1` stage catalog exposes the following editable settings; all other Python runtime settings remain process configuration in installed defaults and the [configuration guide](../lib.md). `context` and `safety` have no graph-editable settings in this first catalog: context budgets come from validated process config and safety policy remains in `safety/gate.toml`, outside arbitrary node edits. The stage type owner is EGIA; IsOS validates the saved values and render request, then EGIA validates them again at startup. A missing optional setting uses the installed default; existing values keep their names and meaning. Adding a setting requires an optional field with a default or a new type ID.

| Stage / setting | Type / unit | Purpose and installed default | Validation and compatibility |
|---|---|---|---|
| `link.backend_alias` | string / none | Stable EOS route; required on each link. | Nonblank, unique across link instances; old single-backend mode normalizes to `default`. |
| `link.gateway` | endpoint reference / none | IsOS-resolved EOS gRPC endpoint; required in managed mode. | TCP host/port or absolute UDS path; HTTP control/catalog endpoint rejected; old TCP profile without `transport` remains TCP. |
| `perception.capacity` | integer / records | Admission FIFO bound; 64. | `1..10000`; overflow counts and reports omissions. |
| `perception.history_per_kind` | integer / records per kind | Prompt history depth; 8. | `1..10000`; changing it does not reinterpret stored events. |
| `missions.mission_capacity` | integer / pending missions | Queue admission bound; 8. | `1..1024`; full queue rejects a new mission. |
| `decision.controller.model` | selector ID / none | Fast model binding; `gemma4-fast`. | Nonblank, present in `models.toml`, distinct from reasoner binding; unavailable saved ID remains visible but blocks Apply. |
| `decision.reasoner.model` | selector ID / none | Slow model binding; `gemma4-reasoning`. | Same validation as controller and must differ from it. |
| `decision.max_invalid_replies` | integer / completions | Consecutive invalid model replies before mission failure; 3. | At least 1; transport retry does not consume this count. |
| `decision.step_timeout_s` | finite number / seconds | Primitive step egress timeout; 30. | Greater than 0; timeout is not EOS completion. |
| `monitor.heartbeat_interval_s` | finite number / seconds | Backend health poll interval; 5. | Greater than 0; edge frequency does not replace this safety monitor cadence. |
| `monitor.failure_threshold` | integer / polls | Consecutive heartbeat failures before escalation; 3. | At least 1; reset only after verified recovery. |
| `monitor.stall_timeout_s` | finite number / seconds | Running mission without a fresh primitive result before deviation; installed default 150. | Greater than 0; model inference in flight is not treated as a primitive stall. |

`server.port` is a process-level integer TCP port (1–65535, installed default 16060), not a stage setting. `gateway.transport` is `tcp` or `uds`; transport-conditional fields remain explicit in the [IsOS config adapter](isos-config.md). A selector's stable `value` is stored in the graph, while package or config resolution produces an actual path outside the graph. Selector options must include label, description, availability, and unavailable reason; the UI preserves an unavailable saved value visibly and blocks Apply until corrected. The first release uses configured model bindings; package-managed weight selection is a later extension and does not add `egia.tensor` now.

## Admission, error, and compatibility behavior

**Normal:** A complete discovery generation makes link ready; evidence moves through perception and context; a selected mission produces a proposal; the gate either admits one exact primitive or emits a rejection; EOS terminal result returns as evidence. **Empty:** no mission or optional observation leaves stages idle without inventing data. **Malformed, duplicate, stale:** reject at the owning stage with stable event code, node ID, and correlation ID. **Unavailable and partial:** required discovery blocks readiness, optional model/memory sources degrade. **Timeout and cancellation:** preserve pending or failed state until terminal EOS evidence; never infer physical completion. **Restart:** load checkpoints, create a new run ID, rediscover EOS, and keep the emergency latch until authorized reset.

The source-edge scheduler may prioritize and cap evidence delivery, but gate admission always uses the current validated state. Overflow of a bounded observation or context queue is counted and logged; safety and terminal result events are never silently discarded. A graph cannot connect a model proposal directly to link or configure an arbitrary emergency sender. The only emergency bypass is monitor's dedicated stop egress.

## Interface verification

The [Q03 owner qualification design](../design/q03-qualification.md) fixes a measurement of the two existing A04 evidence routes. Its workload and metrics are test evidence, not additional stage ports, QoS settings, or safety routes. The selected installed-wheel and cross-owner replay remain release gates.

The [Q01 Vision evidence ingress](vision-evidence.md) is a separate process-owned, read-only inspection path. It adds no eighth stage, graph port, A04 `resource` image interpretation, prompt input, or actuator route. A gate frame never enters this stage scheduler in Q01.

Use contract fixtures for each port and setting descriptor, unknown-field/additive-field compatibility, legacy `@v1.0` read-only interpretation, invalid future type, unavailable selector value, and malformed/stale/duplicate events. A stubbed EOS test must prove that graph wiring cannot bypass the gate and a disconnect cannot produce a successful result. Keep full test output under `logs/test/<run-id>/` when implemented.

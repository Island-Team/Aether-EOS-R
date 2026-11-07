# EOS v2 primitive bindings and typed command envelope

[中文](../../../docs_cn/eos/interfaces/primitive-bindings_cn.md) · [Hardware profile](hardware.md) · [Graph controls](control.md)

An EOS primitive binding names one versioned movement operation and connects it to exactly one hardware device. The IsOS graph owns the binding edge; EOS reads it, validates a typed command header, and sends the decoded Rust movement through the shared Dispatcher. The binding does not own a driver or authorize direct actuator release.

## Graph contract

The built-in types `eos.dispatcher.primitive_base@v1.0` and `eos.dispatcher.primitive_manipulator@v1.0` live at `eos/dispatcher/base/<id>` and `eos/dispatcher/manipulator/<id>`. Each has one `command` output of `base_movement` or `manipulator_movement`. A generic `eos.hardware.base@v1.0` or `eos.hardware.manipulator@v1.0` device has `inputs.command` with cardinality `many`: an explicit array of one or more `{node,output}` sources. Every binding must appear on exactly one compatible device input. The binding has no target setting; the edge is its target. The shared Dispatcher service is implicit for this route, so a graph need not contain `eos.dispatcher.control@v1.0`.

```json
{"id":"velocity","node_type":"eos.dispatcher.primitive_base@v1.0","label":"Base velocity","description":"Bounded velocity binding","settings":{"primitive_name":"base.velocity","primitive_version":"1.0.0","movement_type":"body_velocity"}}
```

```json
{"command":[{"node":"eos/dispatcher/base/velocity","output":"command"}]}
```

| Field | Type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `settings.primitive_name` | string | primitive ID | Required; currently `base.velocity` for base or `arm.movej` for manipulator. |
| `settings.primitive_version` | string | semantic version | Required; currently `1.0.0` for both admitted primitives. |
| `settings.movement_type` | string | mode ID | Required; `body_velocity` for `base.velocity`, `joint_positions` for `arm.movej`; the device must advertise the mode. |
| `inputs.command` | array of `PortSource` | typed graph edges | Required for an actuator; each source is distinct, type compatible, and a binding output. One binding targets exactly one device. |
| `PortSource.node` | string | canonical path | Source instance `<core>/<layer>/<group>/<node>`; must exist. |
| `PortSource.output` | string | output port | Must name the source's registered `command` output. |
| `node.adapter.type` | string | family ID | Selects one device-owned compiled factory such as `mujoco-v2` or `mock-cxx`. |
| `node.adapter.package` | `{id:string,version:string}` or absent | package reference | Required for a package family; exact verified package pin, independent of the built-in node type's `node.package`. |
| `node.adapter.settings` | object | family specific | Validated device endpoint settings; MuJoCo uses `backend`, `socket_path`, `simulator_node`, and `device_id`. |

The registry catalog includes `primitive_specs` with `name`, `version`, `device_class`, `movement_type`, and `description`. It includes `adapter_families` with `id`, `device_kinds`, `transport`, `description`, `supported_primitives`, optional `settings_schema`, optional exact `package`, and EOS-computed `compiled` and `reason`. Package sidecars may declare the family fields but cannot assert that code is compiled. `GET /api/eos/v2/primitive-specs` and `GET /api/eos/v2/adapter-families` expose these catalogs. `aether-profile catalog --node-types <verified-sidecar>` exposes the same data. The compiled `mujoco-v2` family admits `island/mujoco-sim@0.1.1` camera, base, and manipulator devices. One adapter owns a physical device and its typed methods, including stop; a primitive does not create another adapter.

The planned IsosUI EOS panel projects **one control per active binding instance**, not one control per primitive name. It resolves the binding's one target from the active graph and shows both the type description and instance description. It may use the declared primitive catalog and hardware limits to build a typed argument form; it must not let a draft binding, matching display name or user-supplied target create another route. The [layer data-flow demo](../../island/layer-dataflow.md) owns the operator sequence and feedback state.

## Bounded runtime command

The EOS owner-only Unix control socket currently accepts one JSON line, up to 64 KiB. `primitive_typed` is the command operation for new bindings. Its `payload` is a typed movement object from [hardware v2](hardware.md); the JSON bytes counted by the **private** `payload_length_bytes` are the compact `serde_json` UTF-8 serialization of that parsed object. The loopback HTTP bridge accepts a bounded public compact UTF-8 length and normalizes the private header after parsing because JavaScript and Rust can serialize the same finite number with different byte lengths; see [the bridge contract](control.md#loopback-api). The decoder rejects unknown fields, lengths above 16 KiB, unsupported codec or version, a forged graph target, and a primitive/payload mismatch before asking Dispatcher to admit the command. Dispatcher still checks health, deadlines, limits, journaling, and stop.

For a browser invocation with the merged EOS R03 implementation, the existing loopback `/api/eos/v2/control` bridge allowlists `primitive_typed` and forwards it to this private runtime operation after Origin and size checks. A caller can send `deadline_after_ms`, which EOS resolves against its own monotonic clock. Existing absolute-deadline callers obtain EOS monotonic time from the bridge. EOS validates the typed header, exact active binding and target again. The response's `command_id`, exact `Completed`/`Cancelled`/`Rejected`/`InProgress`/`OutcomeUnknown` status and reason drive the panel's result display; browser-local sending is only a pending indicator. The browser bridge rewrites a bounded public `result` to guarded private `primitive_result`: the [R07 authorized result contract](control.md#r07-authorized-browser-result) defines the bounded read needed before the UI follows `InProgress`. An uncertain outcome is never shown as completed. This is the UI ingress to the existing contract, not a new graph wire or gRPC service name.

```json
{"op":"primitive_typed","header":{"data_type_id":"base_movement","data_type_version":"1.0","primitive_name":"base.velocity","primitive_version":"1.0.0","source_path":"eos/dispatcher/base/velocity","target_path":"eos/hardware/base/drive","command_id":"move-1","sequence":1,"deadline_monotonic_ns":5000000000,"payload_length_bytes":83,"codec":"json_v1"},"payload":{"type":"body_velocity","x_m_s":0.1,"y_m_s":0.0,"yaw_rad_s":0.0,"valid_for_ms":100}}
```

The example above records the historical R02 request shape and is rejected for movement by the merged EOS R03 implementation because it lacks the active revision. Under the [R03 managed admission](grpc.md#r03-governed-actuator-admission-target-contract), `header.active_revision` is required and equals the IsOS saved whole-graph `active_revision`; the supervised bridge adds an outer `authority_grant` itself over private IPC, and browsers never send or see that grant. The runtime rejects a missing/stale revision or wrong grant before any adapter call. The merged EOS bridge injects its private operator grant; R07 still owns authorized result follow-up and full browser-path qualification. For direct private IPC, `payload_length_bytes` must equal the sender’s compact `serde_json` bytes. For public HTTP, the browser sends a bounded 1–16,384 byte hint; the bridge rewrites the private field from its own parsed-payload serialization before runtime validation. The displayed historical value is illustrative. Success returns `{"ok":true,"result":<DispatchResult>}`. A rejected header returns `{"ok":false,"error":"target_path differs from the graph binding edge"}` or another bounded validation error. No adapter call occurs on a header error.

| Header field | Type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `data_type_id` | string | registered ID | `base_movement` or `manipulator_movement`, matching decoded payload and binding port. |
| `data_type_version` | string | IDL version | Required `1.0`. |
| `primitive_name` | string | primitive ID | Must equal the binding's `primitive_name`. |
| `primitive_version` | string | semantic version | Must equal the binding's `primitive_version`. |
| `source_path` | string | canonical graph path | Must name the exact binding node in short form. |
| `target_path` | string | canonical graph path | Must equal the one hardware device selected by the graph edge. |
| `active_revision` | string | saved-byte SHA-256 | Required 64 lowercase hex digits for managed movement, equal to EOS admitted IsOS active revision; absent in the historical R02 example, which now fails before adapter release. |
| `command_id` | string | command identity | Nonempty, at most 128 UTF-8 bytes; Dispatcher also enforces its ID grammar and duplicate policy. |
| `sequence` | unsigned integer | command sequence | Required positive value; caller supplies it for ordering and tracing. |
| `deadline_monotonic_ns` | unsigned integer | ns | EOS monotonic deadline; Dispatcher rejects expired work. |
| `deadline_after_ms` | unsigned integer | ms | Alternative to the absolute deadline for the HTTP bridge; EOS resolves 1–5,000 ms against its own monotonic clock. Both deadline fields cannot appear together. |
| `delivery_mode` | string | — | `one_shot` by default; `stream` publishes a bounded movement lease through a declared gateway edge. |
| `run_id` | string | — | Required for a stream; must equal the active managed run, including after a restart with an unchanged revision. |
| `gateway_path` | string | graph path | Required for a stream; identifies the declared EOS HTTP gateway connected to the primitive binding input. |
| `payload_length_bytes` | unsigned integer | bytes | Private IPC: exact compact `serde_json` payload length, 1 through 16,384. Public HTTP: bounded hint, normalized by the bridge before private IPC. |
| `codec` | string | codec ID | Required `json_v1`; EOS decodes into `Movement` before CXX methods. |

### Stream movement

The IsOS HTTP bridge sends `delivery_mode:"stream"`, the active `run_id`, a declared `gateway_path`, and `deadline_after_ms` with each `base_movement` command. EOS accepts a stream only for `body_velocity` and checks the active revision, run, and graph edges before publication. A subsequent `primitive_result` may report `Completed` with reason `stream lease published`: that acknowledges delivery to the hardware adapter, not physical stationarity. Each command replaces the previous bounded lease; a zero-velocity command promptly replaces held motion on key release. The gateway and primitive binding declare `max_rate_hz:20` for this path.

## Compatibility and channels

Graph import accepts the former `cores/<core>/layers/<layer>/groups/<group>/nodes/<node>` form and rewrites all references and edges to short paths atomically. A graph mixing path forms fails validation. A legacy single-object `inputs.command` is imported as an array. The old `primitive` operation and `eos.control.primitive@v1.0` remain parseable for existing graphs. The merged EOS R03 implementation rejects the old operation on the deployed private socket; new binding nodes use governed `primitive_typed`. Historical in-process compatibility helpers remain outside deployed ingress. The camera's high-rate path uses the existing bounded latest-value typed hub and dedicated read-only Vision WebSocket stream. High-rate actuator channels remain future work; the current command path is a low-rate multiplexed Unix control channel.

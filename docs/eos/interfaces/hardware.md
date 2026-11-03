# EOS node profile and typed hardware data

[中文](../../../docs_cn/eos/interfaces/hardware_cn.md) · [Package node IDL](node-contributions.md) · [Hardware design](../design/runtime/hardware.md) · [Dispatcher design](../design/runtime/dispatcher.md)

This reference describes the currently implemented profile in `aether-node-profile` and typed hardware data in `aether-hal`. The [IsOS graph contract](https://github.com/Island-Team/isos/blob/master/docs/isos/interfaces/node-graph.md) owns the graph envelope, revisions, migration, and wire QoS; this page owns EOS hardware and node-type admission. Rust now reads graph `1.0.0` with established types and graph `0.1.0` with registered `@v0.1` types; IsOS migration and cross-reader preflight remain separate work. `Option<T>` means a value or capability may be absent; it must not be fabricated. `Vec<T>` is ordered. Unless otherwise stated, numeric values must be finite.

## Profile layout

The profile is one `core → layer → group → node` tree. Core is the lifecycle authority, layer is a domain, and group is a type family. Each node instance names a registered type; that type fixes its placement, kind, lifecycle, allowed capabilities, settings, and typed ports. Readers accept `1.0.0` with `@v1.0` types and `0.1.0` with `@v0.1` types. A hardware address is `eos/hardware/<camera|base|manipulator>/<node_id>`. Legacy eight-segment paths are imported and rewritten atomically; new graphs emit short paths. `NodeKind` is resolved from the type, not inferred from the path. This legacy example has an EOS camera, an EOS dispatcher, and an EGIA request guard:

```toml
version = "1.0.0"
[robot]
id = "robot-1"
name = "Robot One"
[cores.eos.layers.hardware.groups.camera.nodes.front]
id = "front"
node_type = "eos.hardware.camera@v1.0"
label = "Front camera"
description = "RGB and registered depth"
frame_id = "front_optical"
capabilities = ["rgb8", "aligned_depth_u16"]
[cores.eos.layers.hardware.groups.camera.nodes.front.adapter]
type = "mock-cxx"
[cores.eos.layers.hardware.groups.camera.nodes.front.health_timing]
poll_interval_ms = 100
stale_after_ms = 500
[cores.eos.layers.hardware.groups.camera.nodes.front.observation]
max_rate_hz = 30.0
max_payload_bytes = 16777216
[cores.eos.layers.dispatcher.groups.control.nodes.command]
id = "command"
node_type = "eos.dispatcher.control@v1.0"
label = "EOS dispatcher"
description = "Admits commands to configured hardware nodes"
capabilities = ["admission", "status"]
references = ["eos/hardware/camera/front"]
[cores.egia.layers.common.groups.process.nodes.guard]
id = "guard"
node_type = "egia.common.guard@v1.0"
label = "EGIA request guard"
description = "Design-only guard for EOS control requests"
capabilities = ["request_guard"]
references = ["eos/dispatcher/control/command"]
```

| Field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `version` | `String` | profile version | Reader accepts exact `"1.0.0"` and `"0.1.0"`; IsOS alone writes a migrated graph. |
| `robot` | `RobotProfile` | — | Names the robot to which this node tree belongs; required. |
| `robot.id` | `String` | — | Stable robot identity; required address segment containing only ASCII letters, digits, `_`, or `-`. |
| `robot.name` | `String` | — | Human display name; required nonblank. |
| `cores` | `BTreeMap<String, CoreProfile>` | — | Top-level execution authorities, such as `isos`, `eos`, or `egia`; each key is a valid address segment and each core has at least one layer. |
| `cores.<core>.layers` | `BTreeMap<String, LayerProfile>` | — | Domains owned by a core; nonempty, with valid segment keys. Registered node types determine allowed placement. |
| `...layers.<layer>.groups` | `BTreeMap<String, NodeGroup>` | — | Type families within a domain; nonempty, with valid segment keys. Registered node types determine allowed placement. |
| `...groups.<group>.nodes` | `BTreeMap<String, NodeProfile>` | — | Node identities in a group; nonempty, with valid segment keys equal to `node.id`. Full paths are unique; EOS hardware IDs are additionally unique across hardware groups for adapter registration. |
| `node.id` | `String` | — | Stable address segment; nonempty ASCII letters, digits, `_`, or `-` only, and equal to map key. |
| `node.node_type` | `String` | registered type ID | Required reference to a registered `@v1.0` definition. Its core/layer/group must match the instance path; unknown type or version fails validation. |
| `node.package` | `Option<PackageRef>` | package ID and version | Required for a package-contributed type and equal to its verified sidecar's `{id, version}`; omitted for built-in types. ID is `owner/name` with valid address segments; version has three numeric components. |
| `node.label` | `String` | — | Short operator label; required nonblank. |
| `node.description` | `String` | — | Explains the node's role; required as a field, may be empty. |
| `node.frame_id` | `String` | coordinate frame | Names hardware observations and spatial commands; required nonblank for hardware, omitted for software. |
| `node.joint_names` | `Option<Vec<String>>` | — | Static ordered joint authority; required for manipulator and forbidden for all other nodes. Nonempty, unique address-segment names. Observations and joint targets must match this order. |
| `node.adapter` | `AdapterProfile` | — | Hardware adapter selection and settings; hardware must supply a nonblank type, software must leave its default values. |
| `node.adapter.type` | `String` | — | Selects the C++ or mock hardware adapter factory; required nonblank for hardware, omitted for software. |
| `node.adapter.package` | `Option<PackageRef>` | package ID and version | Exact provenance for a package adapter family such as `mujoco-v2`; independent of `node.package`, which pins a contributed type. Omitted for `mock-cxx`. |
| `node.adapter.settings` | `BTreeMap<String, toml::Value>` | adapter-specific | Optional hardware adapter settings; default empty; the selected adapter must validate its own keys and bounds. Software nodes cannot declare these settings. |
| `node.capabilities` | `Vec<String>` | — | Advertised functions for discovery; optional for software, nonempty for hardware, each a unique valid address segment. Camera requires `rgb8` and may add `aligned_depth_u16`; base/manipulator capabilities match their declared movement modes. It does not claim current health. |
| `node.health_timing` | `HealthTiming` | — | Polling and staleness policy for hardware health; hardware requires valid intervals, software leaves default zero values. |
| `node.health_timing.poll_interval_ms` | `u64` | ms | Hardware health polling interval; greater than zero for hardware, omitted for software. |
| `node.health_timing.stale_after_ms` | `u64` | ms | Age at which a hardware health report becomes stale; at least `poll_interval_ms`, omitted for software. |
| `node.observation` | `Option<ObservationProfile>` | — | Hardware read bounds; required for camera, base, and manipulator, forbidden for software. The Rust type remains optional so validation can report a missing section. |
| `node.observation.max_rate_hz` | `f64` | Hz | Maximum sampling rate for admission and resource sizing; finite and greater than zero. |
| `node.observation.max_payload_bytes` | `u64` | bytes | Maximum sample payload before allocation or transfer; greater than zero. |
| `node.movement` | `Option<MovementProfile>` | — | Allowed movement modes and limits; required for base/manipulator and forbidden for camera/software. |
| `node.movement.modes` | `Vec<String>` | — | Nonempty, unique list. Base: `body_velocity`, `relative_pose`; manipulator: `joint_positions`, `end_effector_pose`. Each mode requires its matching capability and limits. |
| `node.movement.limits` | `MovementLimits` | — | Numeric ceiling set; every present value must be finite and greater than zero. |
| `limits.max_linear_m_s` | `Option<f64>` | m/s | Bounds base translational velocity. |
| `limits.max_angular_rad_s` | `Option<f64>` | rad/s | Bounds base yaw rate. |
| `limits.max_translation_m` | `Option<f64>` | m | Bounds relative base translation. |
| `limits.max_rotation_rad` | `Option<f64>` | rad | Bounds relative base yaw. |
| `limits.max_joint_position_rad` | `Option<f64>` | rad | Bounds magnitude of a requested joint position. |
| `limits.max_joint_speed_rad_s` | `Option<f64>` | rad/s | Bounds requested joint speed. |
| `limits.max_end_effector_speed_m_s` | `Option<f64>` | m/s | Bounds requested Cartesian speed. |
| `limits.max_velocity_lease_ms` | `Option<u64>` | ms | Profile ceiling for one body-velocity lease; required for `body_velocity`. Admission also requires the lease to fit within the remaining command deadline. |
| `limits.max_end_effector_translation_m` | `Option<f64>` | m | Bounds the Euclidean norm of an end-effector target's XYZ position from its frame origin; required for `end_effector_pose`. |
| `node.references` | `Vec<String>` | canonical node paths | Optional directed references to other nodes, for example guard to dispatcher; default empty. Each full path must exist in this profile, be unique within the list, and differ from the referring node's own path. A reference grants no hardware ownership or permission by itself. |
| `node.settings` | `BTreeMap<String, toml::Value>` | type-specific | Instance configuration; default empty. Registry rejects unknown keys, missing required keys, wrong value types, and invalid values. IsOS process settings are listed below. |
| `node.inputs` | `BTreeMap<String, PortSources>` | typed edges | Input-port connections keyed by receiving port. A source path and output must exist, match the declared data type, and differ from the receiving node. Base/manipulator command input has `many` cardinality: a nonempty array of primitive binding outputs; each binding targets exactly one device. Legacy dispatcher edges remain accepted during migration. Other inputs follow their registered cardinality and required flag. |
| `node.inputs.<port>[i].node` | `String` | canonical node path | Source instance for a typed connection; must exist and differ from the receiving node. Both cardinalities accept an object or array for legacy reads; new multi-source authoring emits an array. |
| `node.inputs.<port>[i].output` | `String` | output port name | Source type's output port; must exist and match the receiving port's data type. |
| `node.inputs.<port>[i].priority` | `Option<i64>` | nice scale | Optional user delivery priority; default `0`, inclusive `-20..19`, allowed only on eligible EOS-local evidence edges. An explicit value on a fixed or unrouted path fails. R01 uses weight `20 - priority`. |
| `node.inputs.<port>[i].frequency_hz` | `Option<f64>` | Hz | Optional user delivery cap; absent means no edge cap. If present it must be finite and greater than zero on an eligible EOS-local evidence edge; fixed or unrouted paths reject it. `Port.expected_rate_hz` is only a hint. |
| `node.kind` | `NodeKind` | — | Derived from the registered type as `Camera | Base | Manipulator | Software`; not authored in TOML. |
| `node.path` | `String` | registry path | Derived canonical `<core>/<layer>/<group>/<id>` address after validation; not authored in TOML. |

Software nodes may declare identity, label, description, capabilities, and references; hardware fields must remain at their absent or default values and should be omitted in TOML. An absent limit for an unadvertised hardware mode does not imply safe unlimited physical movement: adapter and controller physical bounds still apply. All limits required for an advertised mode must be present. The dispatcher checks configured values before release.

## Node type registry and typed connections

The built-in type IDs are `eos.hardware.camera@v1.0`, `eos.hardware.base@v1.0`, `eos.hardware.manipulator@v1.0`, `eos.dispatcher.control@v1.0`, the three [graph control types](control.md), the four `egia.common` types below, `isos.process.component@v1.0` and `isos.process.component@v1.1`, and `eos.process.backend@v1.0`. `Profile::from_toml` and `Profile::from_json` use these built-ins; `*_with_registry` accepts a caller-supplied registry. `NodeTypeRegistry::all()` supplies a serializable catalog for graph editors. Registration rejects duplicate IDs, invalid placement and fields, and custom actuator types without a compatible registered safety adapter. A registered type does not itself instantiate that adapter; runtime admission must resolve it.

| Registry field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `NodeType.id` | `String` | type ID | Unique `<core>.<layer>.<name>@v<major>.<minor>`; current built-ins use `v1.0`. Registered before graph validation. |
| `NodeType.description` | `String`, decoded default `""` | — | Implemented: new `@v0.1` types require a nonblank purpose; old sidecars may omit it and receive an in-memory catalog fallback without changing verified bytes. Built-in types supply a description. |
| `NodeType.core`, `.layer`, `.group` | `String` | address segments | Required execution authority, domain, and family. Each is a valid segment; instance placement must match. |
| `NodeType.kind` | `NodeKind` | — | Determines hardware data and adapter validation. Hardware types must be EOS hardware adapter types. |
| `NodeType.lifecycle` | `Lifecycle` | — | `Internal | Process | Adapter`; identifies who starts and stops the instance. |
| `NodeType.capabilities` | `Vec<String>` | capability IDs | Upper bound for instance advertisements; each ID is a valid segment. |
| `NodeType.settings` | `Vec<Setting>` | — | Allowed instance settings; setting names must be unique. |
| `NodeType.inputs`, `.outputs` | `Vec<Port>` | — | Typed connection surface; names unique within each direction. |
| `NodeType.safety_adapter_type` | `Option<String>` | adapter ID | Required when registering a custom base or manipulator type; must name a registered safety adapter of the same kind that covers its movement modes and can stop. |
| `SafetyAdapterType.id` | `String` | adapter ID | Unique valid segment used by a custom actuator type. |
| `SafetyAdapterType.kind` | `NodeKind` | — | `Base` or `Manipulator`; must match the custom actuator. |
| `SafetyAdapterType.movement_modes` | `Vec<String>` | mode IDs | Nonempty set of modes covered by safety checks; must cover the custom actuator's capabilities. |
| `SafetyAdapterType.supports_stop` | `bool` | — | Must be true at registration; runtime implementation must perform an actual stop. |
| `Setting.name`, `.data_type`, `.required` | `String`, `SettingType`, `bool` | — | Named setting schema: `String`, `PositiveInteger`, `NonnegativeInteger`, `StringArray`, or `StringTable`; required settings must be present. |
| `Port.name`, `.data_type`, `.required` | `String`, `PortDataType`, `bool` | — | Named typed input or output; required input ports must be connected. Data types are specific hardware frame, observation, movement, outcome, health, status, or control-request types. |
| `Port.cardinality` | `PortCardinality` | connection count | `one` by default or `many`; a required `many` input needs a nonempty array, with distinct compatible sources. |

### G06 typed edge admission

IsOS owns the draft and edge-addressed Apply diagnostics. EOS validates its own applied edges before constructing a route or adapter. `node.inputs.<port>` accepts a source object or an array of source objects for compatibility with legacy graphs. A `one` input contains exactly one source in either form; a `many` input contains at least one distinct source, and new authoring emits an array. `node` is a canonical four-segment path, `output` is a registered output name, optional `priority` is an integer on the Linux nice scale `-20..19` (default `0`), and optional `frequency_hz` is a finite positive number in hertz (absent means uncapped). These fields describe delivery only; they never submit an actuator command. The wire representation retains omitted legacy fields without rewriting the IsOS-owned graph.

For every EOS-owned source, require an existing non-self node at its registered core/layer/group placement, an exact source output and destination input data-type ID, a compatible transport declaration, valid input cardinality, distinct sources, and all required inputs. The one registered data-type descriptor owns each payload schema and its `x-unit` annotations; duplicate IDs are rejected at registration. Unit equality follows from exact type identity, not conversion. A legacy port with no transport declaration means `in_process`; a new `@v0.1` type declares transport explicitly. EOS rejects cycles among EOS-owned edges and invalid EOS references. IsOS validates the full cross-core graph, EGIA-owned edges, declared bridges and IsOS process dependencies, returning the input source index when present. Runtime health is live state and does not change whether a draft may be edited or applied.

IsOS classifies admitted edges as `eos_local`, `egia_local`, `service_bridge`, or `design_only`; EOS uses only its `eos_local` projection. EOS-local evidence can be routed only when EOS has a compiled producer, consumer and transport; `egia_local` belongs to EGIA; the Q01 camera-gate `preview` to EGIA Vision evidence ingress is a selected IsOS-managed read-only service binding outside authored graph edges, not an admitted `service_bridge`. The legacy `isos.ui.vision@v1.0` preview is not a `0.1.0` destination. Historical graph-authored control and other accepted descriptive edges remain `design_only` until an owner implements delivery. IsOS rejects a cross-core edge without an explicit registered bridge. No planner proposal, contributed type, or generic command port can create an actuator path outside Dispatcher.

Only an eligible routed evidence edge may carry user `priority` or `frequency_hz`: EOS-local image/observation/state/event edges and EGIA-local evidence edges when their owner implements routing. Fixed command, primitive-binding, safety, stop, result, health, readiness and latch paths, service bridges, and design-only edges reject either field even if the supplied priority is `0`. In particular, a base or manipulator `inputs.command` source must be a compatible registered primitive binding at exactly one device; the Dispatcher remains the only command admission and stop owner. IsOS reports the exact JSON pointer to an invalid field or `/inputs/<port>/<source-index>`; EOS validates its own projection before runtime construction. R01 adds live queueing and `20 - priority` weights without changing this admission rule.

### R01 live EOS-local evidence routes — target contract

**Implemented in the R01 topic branch.** R01 constructs a live table only from the validated `EosEdgeContract` returned by `Profile::eos_edge_contract`. In this release the sole eligible graph route is `eos.hardware.camera@v0.1.frame` → `eos.interface.camera_gate@v0.1.frame`, with `camera_frame`, `image`, and `in_process` identities. Both exact endpoints must be present in the active EOS projection and compiled producer/consumer must exist. A similar type, contributed port, legacy `@v1.0` camera gate, EGIA-local declaration or prospective Vision bridge has no R01 delivery. Later routes need a reviewed EOS registry entry and matching IsOS G06 admission update. `VisionService` captures once per camera source and feeds each declared `@v0.1` gate through this table, without creating a second camera capture or actuator command path.

For each active route, the stable key is `(source_node,source_output,target_node,target_input)` within the EOS projection SHA-256. Build order is bytewise key order; reject a duplicate key, missing compiled endpoint or unsupported data class before starting route workers. `priority` defaults to `0` and maps to integer `weight = 20 - priority` (`-20` → `40`, `0` → `20`, `19` → `1`). `frequency_hz` is optional, finite and positive as G06 admits; absence means no edge cap. The cap is a delivery ceiling, not a sampling request or producer `expected_rate_hz`. A capped route starts with one token and consumes it on an attempted delivery, including a consumer error. For a later token, require positive monotonic nanosecond elapsed time since the last consumed token; if `rate <= 1e9`, compare `(elapsed_ns as f64) * rate >= 1e9`, and if `rate > 1e9`, one token is available at the next distinct nanosecond. The bounded product is finite even for the largest elapsed `u64` value. Never compute `1 / rate`, convert the period to an integer duration, or accumulate beyond one token. A backward clock observation adds no token and does not move the last-consumption timestamp. This admits tiny positive subnormal and maximal finite `f64` values without overflow, division by zero, a zero-duration busy loop, or an unbounded burst. IsOS retains original graph bytes and owns Apply; EOS does not rewrite them.

Queue policy is selected by registered `DataClass`. `image` and `observation` retain exactly one latest payload per route; a newer accepted payload replaces the old one and increments `replaced_count` and `gap_count`. `state` and `event` use FIFO of at most 64 records and 1 MiB serialized payload bytes per route; if either bound would be exceeded, reject the newest record and increment `overflow_count` and `gap_count`, preserving earlier order. An oversized record also counts as overflow. Across all live routes, at most 256 routes and 64 MiB queued payload bytes are permitted; startup rejects too many routes, while later global-byte pressure rejects the newest candidate with overflow and gap counts, preserving any retained older sample. For a latest queue, test the net byte change before replacing its prior sample. The 0.x owner registry has no live state/event route, so their queue behavior is exercised with synthetic scheduler tests and does not authorize a graph edge. `command`, fixed safety/result/health/stop and `opaque` data never enter these queues. A consumer failure removes that evidence payload, increments `consumer_error_count` and `gap_count`, and never claims delivery; evidence is not silently retried. All route counters saturate at `u64::MAX` and reset only with a new runtime generation.

The existing owner-only runtime Unix socket gains `{"op":"route_status"}`. Its success reply is `{"ok":true,"projection_sha256":"<64 lowercase hex>","routes":[...]}`; it contains no frame or command payload. `projection_sha256` identifies the exact EOS projection bytes, not IsOS's saved-byte revision. The response lists validated **EOS-local** declarations only: both source and target canonical paths start with `eos/`. The legacy admitted `egia.common.guard@v1.0` → `eos.dispatcher.control@v1.0` cross-core `DesignOnly` contract may be returned by `Profile::eos_edge_contract` for validation, but must not become an EOS route-status row; IsOS's whole-graph inspector owns that declaration. The EOS-local rows include design-only and fixed paths in stable key order. The socket serves this operation only while the runtime is active; quiescing closes or rejects it, and after exit no stopped snapshot is available. A malformed request returns the existing `{"ok":false,"error":"..."}` owner-socket shape without leaking payloads. IsOS status remains the live deployment authority; future R02/U02 readers may project these counters without interpreting a log line as health.

| Response field | JSON type and unit | Purpose and validation |
| --- | --- | --- |
| `ok` | Boolean | `true` only for a complete snapshot. |
| `projection_sha256` | string, lowercase hexadecimal | Exactly 64 characters, digest of the validated EOS projection bytes; not the IsOS saved revision. |
| `routes` | array of objects | All validated EOS-local declarations, sorted bytewise by route key; both endpoints start with `eos/`, at most 256 may be active R01 routes, and no payload is included. |
| `source_node`, `target_node` | string, canonical four-segment EOS path | Existing admitted endpoints; each segment passes profile path validation. |
| `source_output`, `target_input` | string, registered port name | Exact registered port names; together with endpoint paths form the unique route key. |
| `data_type` | string, registered type ID | Exact common output/input type; no inferred conversion. |
| `delivery_state` | enum string | `active` for this live route, `not_routed` for an EOS-local design-only declaration, or `fixed_not_scheduled` for a fixed owner path; no stopped snapshot is served and none represents node health. |
| `routed` | Boolean | `true` while an admitted R01 delivery route is installed and enabled, even before its first frame; otherwise `false`. |
| `priority` | integer or null, nice scale | Active route's admitted `-20..19`, default `0`; null for non-R01 rows. |
| `frequency_hz` | finite positive number or null, Hz | Active route's optional delivery ceiling; null means no cap or non-R01 row. Interpret with `routed`. |
| `weight` | integer or null, scheduler credit | Exactly `20 - priority` for active rows (`1..40`); null otherwise. |
| `queue_class` | `latest`, `fifo`, or null | Registered data class maps image/observation to `latest`, state/event to `fifo`; null for non-R01 rows. |
| `queue_depth` | nonnegative integer or null, records | Retained records in this live route; null for non-R01 rows. |
| `delivered_count` | nonnegative integer or null, records | Successful consumer acceptances in this runtime generation; null for non-R01 rows. |
| `cap_wait_count` | nonnegative integer or null, visits | Scheduler visits with queued work and no token; waiting is not loss. |
| `replaced_count`, `overflow_count`, `consumer_error_count`, `cancelled_count`, `gap_count` | nonnegative integer or null, records | Distinct loss reasons and aggregate missing-record count for this generation; null for non-R01 rows. Counters saturate at `u64::MAX`. |
| `last_delivery_monotonic_ns` | nonnegative integer or null, ns since runtime boot | Time of last consumer acceptance; null before first acceptance or for non-R01 rows. Not a wall clock. |

The request and an illustrative first snapshot for the [G06 evidence fixture](../../../tests/fixtures/edge_contract/valid_evidence.json) are:

```json
{"op":"route_status"}
```

```json
{"ok":true,"projection_sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef","routes":[{"source_node":"eos/hardware/camera/front","source_output":"frame","target_node":"eos/interface/gate/front","target_input":"frame","data_type":"camera_frame","delivery_state":"active","routed":true,"priority":-20,"frequency_hz":15.0,"weight":40,"queue_class":"latest","queue_depth":0,"delivered_count":0,"cap_wait_count":0,"replaced_count":0,"overflow_count":0,"consumer_error_count":0,"cancelled_count":0,"gap_count":0,"last_delivery_monotonic_ns":null}]}
```

The digest above is an example value, not the fixture's computed digest. A malformed operation returns `{"ok":false,"error":"unsupported op: bogus"}` on the owner socket. Transport failure or a missing route table is a runtime failure, not a successful empty route list.

An active route reports `delivered_count` only after consumer acceptance. `cap_wait_count` counts scheduler visits with queued work but no token; waiting alone is not a gap. `queue_depth` counts retained queued records and excludes the single in-flight consumer attempt per route. `last_delivery_monotonic_ns` is null before the first success. On shutdown EOS atomically closes evidence admission and fences in-flight publication, invokes the Dispatcher safety stop before draining queues or joining any evidence worker, then cancels retained records; R01 never delays actuator stop behind a stalled consumer or a large queue. A ready managed MuJoCo child uses the [routine shutdown marker](simulator-stop.md#routine-shutdown-state-distinct-from-emergency-stop); a genuine `safety.estop` retains its durable latch. Standalone fallback uses `emergency_stop`. Each in-flight consumer uses a cancellation token and a bounded 100 ms acceptance deadline; acceptance before cancellation increments `delivered_count`, while cancellation or deadline expiry increments `cancelled_count` and `gap_count` once and fences later publication. The evidence worker join has a 1 s shutdown deadline; expiry reports a stop failure rather than claiming completion or waiting forever. `cancelled_count` and `gap_count` also increase once per queued record cancelled at shutdown. The sole Dispatcher still owns actuator safety, and R06 owns the full simulator termination/reap ordering and integrated safe-stop proof. [R01 fixtures](../../../tests/fixtures/routed_edges/README.md) freeze live, negative and stop cases.

The final gate acceptance and visible slot publication share the scheduler admission lock: a deadline, cancellation, or sealed scheduler cannot publish a frame or increment delivery. The last-delivery timestamp is sampled at this acceptance commit. The owner seals admission before the Dispatcher safety stop and drains queued records afterward. A positive subnormal camera `max_rate_hz` uses a saturated wait duration and does not panic before readiness.

A synchronous camera adapter capture is outside the 100 ms gate acceptance deadline. If it remains blocked at shutdown, EOS fences publication, waits at most 1 s for the evidence worker, then exits nonzero without emitting a completed-stop event. The blocked OS thread cannot be killed by Rust; the adapter host and R06 simulator boundaries remain responsible for their own termination.

For example, `drive.inputs.command = [{ node = "eos/dispatcher/base/velocity", output = "command" }]` connects a primitive binding output to a `BaseMovement` input. This edge declares the sole graph-owned actuator route; it is not a movement command payload. The current dispatcher addresses the same adapter by validated canonical path. Runtime camera frames and movement commands remain their specific Rust structs below.

The IsOS process type uses `node.settings.command: array<string>` (required nonempty argv), `cwd: string` (optional working directory), `env: table<string,string>` (optional environment), `ready_tcp: string` (optional TCP readiness address), `ready_unix: string` (optional absolute Unix socket readiness path, under 108 bytes), `start_timeout_ms: positive integer` (optional milliseconds), `restart: string` (optional `never | on_failure | always`), `max_restarts: nonnegative integer` (optional count), and `depends_on: array<string>` (optional canonical process node paths; an empty array means no prerequisite). At most one readiness address is allowed. Dependencies must be distinct, present, process typed, non-self, and acyclic. IsOS validates launch semantics when consuming its graph projection.

`isos.process.component@v1.1` replaces `command` with required `role: string` (`eos | egia | isosui` in the EOS registry) and optional `args: array<string>` (nonblank arguments; an empty array is allowed). The current IsOS process projection launches only `eos` or `egia` roles; IsosUI is owned by its workbench service, not a deployable process node. IsOS resolves the executable for supported roles. The other process settings remain available. `@v1.0` remains parseable for migration but its arbitrary command is not launched by IsOS.


### EOS package backend and EGIA common types

`eos.process.backend@v1.0` belongs at `eos/process/backend`. Its settings pin the package and scene. IsOS resolves and verifies the executable and scene; EOS starts and supervises the direct child using these resolved paths. It has no authored `command` setting.

| Setting | Type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `package` | `String` | package ID | Required `owner/name` with valid address segments, such as `island/mujoco-sim`; selects a verified IsOS bundle. |
| `version` | `String` | package version | Required `0.1.1` for the supported `island/mujoco-sim` backend; pins the resolved bundle. |
| `scene` | `String` | scene ID | Required nonblank package scene selection; IsOS checks the selected bundle's scene catalog and assets. |
| `socket` | `String` | Unix socket path | Required absolute path under 108 bytes; IsOS checks its runtime directory and EOS owns the socket server child. |
| `env` | `StringTable` | environment | Optional string map of launch environment values; IsOS constrains names and values at launch. |
| `start_timeout_ms` | positive integer | ms | Optional deadline for simulator readiness in milliseconds; greater than zero and capped at 120,000 by EOS. |

The four design-only EGIA types are `egia.common.source@v1.0` (`egia/common/data`, output `observation: ObservationRef`), `egia.common.context@v1.0` (`egia/common/data`, input `observation: ObservationRef`, output `context: ContextSnapshot`), `egia.common.planner@v1.0` (`egia/common/process`, input `context: ContextSnapshot`, output `proposal: PlanProposal`), and `egia.common.guard@v1.0` (`egia/common/process`, input `proposal: PlanProposal`, output `request: ControlRequest`). These are configuration and typed routing definitions, not an executing EGIA runtime. `egia.tensor` is reserved for later tensor-space data flow; no empty tensor layer is authored.

| EGIA port type | Purpose | Validation |
| --- | --- | --- |
| `ObservationRef` | Names a selected observation source without copying a camera or hardware payload. | Exact port type match; future payload definition remains separate from node settings. |
| `ContextSnapshot` | Carries a bounded, prepared reasoning context. | Exact port type match. |
| `PlanProposal` | Carries a proposed action for guard evaluation. | Exact port type match. |
| `ControlRequest` | Requests EOS dispatcher admission after guarding. | Exact port type match; the dispatcher still owns actuator safety. |

## Shared values and node facets

| Struct.field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `SampleMeta.source_timestamp_ns` | `Option<i64>` | ns | Original source time if available; never used directly for a local monotonic deadline. |
| `SampleMeta.clock` | `ClockKind` | — | `Unix | Ros | Device | Unknown`, identifying the timestamp domain. |
| `SampleMeta.received_monotonic_ns` | `u64` | ns | Local receipt time used for freshness; sampled from unified `aether-common::Clock`. |
| `SampleMeta.sequence` | `Option<u64>` | count | Source sequence when available, for ordering and drop diagnosis. |
| `SampleMeta.frame_id` | `String` | coordinate frame | Frame in which the observation is expressed; must identify the source frame. |
| `Health.state` | `HealthState` | — | `Healthy | Degraded | Unavailable`; observed adapter condition, never profile configuration. |
| `Health.code` | `Option<String>` | — | Stable machine-readable diagnostic code. |
| `Health.message` | `Option<String>` | — | Bounded human diagnostic detail. |
| `Health.last_ok_monotonic_ns` | `Option<u64>` | ns | Last known good local time, for staleness and recovery reporting. |
| `NodeStatus.state` | `NodeState` | — | `Starting | Ready | Stopped | Faulted`, the node lifecycle state. |
| `NodeStatus.active_command_id` | `Option<String>` | — | Correlates an active actuator command; absent when idle. |
| `CommandMeta.command_id` | `String` | — | Caller correlation and duplicate-release protection; required nonblank at dispatch. |
| `CommandMeta.deadline_monotonic_ns` | `u64` | ns | Absolute local monotonic deadline; must be future at admission. |
| `CommandOutcome` | `Accepted | Completed | Cancelled | Ambiguous` | — | Adapter release report; `Ambiguous` means physical execution may have occurred and must not be automatically retried. |

Every adapter implements `NodeAdapter` for `kind`, `adapter_type`, `status`, `health`, and `stop`. The registry checks `adapter_type` against the profile before admission. `CameraNode` adds `capture`; `BaseNode` and `ManipulatorNode` add `observe` plus their typed movement method. A facet is discoverable only for its node kind and advertised mode. The three CXX mock wrappers provide `from_profile(&NodeProfile, Arc<dyn Clock>)` constructors for local integration tests.

R02 adds `NodeAdapter::controlled_stop() -> Result<ControlledStopOutcome, HalError>` for a non-latching stop and `supports_non_latching_stop() -> bool` as its explicit capability proof. The default method is unsupported and the default predicate is false. `ControlledStopOutcome` is `Stopped` (terminal physical proof), `ConfirmedFailure(String)` (known failure), or `OutcomeUnknown` (release cannot be confirmed); the reason is bounded to 256 UTF-8 bytes in the Dispatcher journal. A public `motion.stop` or `robot.stop` requires both this typed proof and `has_terminal_stop_fence()`. The CXX mock's existing `stop()` is permanently latching, so it cannot satisfy either non-latching public stop by name alone; it remains eligible for the distinct `safety.estop` path. Current MuJoCo adapters likewise fail closed until a controlled physical stop is implemented. Dispatcher gives each synchronous physical stop part a bounded wait of at most five seconds across the operation, persists `Releasing` before the call, and retains one in-flight worker per target and stop class after timeout. A timed-out worker cannot be killed by Rust; its later return does not turn the original `OUTCOME_UNKNOWN` into a confirmed result. A detached worker retains journal ownership until it ends, preventing a second runtime from claiming the same durable journal while its physical call remains unresolved. Safety estop has a separate preemptive slot so a blocked controlled stop does not delay emergency admission; adapter implementations must tolerate the two methods being invoked concurrently.

`Dispatcher::dispatch_gateway_stop_checked` returns `StopDispatchOutcome::Operation(DispatchResult)` for a durably admitted or semantically rejected stop. Before any physical release, a journal record capacity failure returns `PreReleaseCapacity` for gRPC `RESOURCE_EXHAUSTED`, and a durable write failure returns `PreReleaseDurability` for gRPC `INTERNAL`. Each variant encloses the same `DispatchResult` for transport-neutral callers. A denied local recovery is a durable terminal `REJECTED` operation and remains an OK gRPC response; a failed persistence step never authorizes a physical call. The compatibility `dispatch_gateway_stop` method returns the enclosed result.

The typed journal operation also stores an optional `GatewayRejectionCode` for terminal admission rejection. `Unauthorized`, `InvalidState`, and `Internal` project as `rejected.unauthorized`, `rejected.invalid_state`, and `rejected.internal`; old records without the field use the stable invalid-state fallback. The code is retained across restart, independent of the human-readable `reason` text. A successful or still-pending operation carries no rejection code.

Durable journal startup binds its control state to the admitted profile robot ID. A stored whole-robot target or bound control ID for another robot fails startup before readiness; an active old control record without a robot-target identity also fails closed. An inactive old control record may bind to the admitted robot and is then persisted with that ID. This prevents a different robot profile from inheriting another robot's stop or estop state.

Movement admission waits for the writer-preferred stop release gate only until the command's remaining monotonic deadline, capped at five seconds. Expired queued workers return a durable rejection and cannot later release movement when the stop gate opens. A cancellation of a Prepared queued command remains `CANCELLED`; it is not turned into a late adapter call.

### Package adapter library ABI

New camera, base, and manipulator families supplied by a verified package may load a Linux shared library. The package manifest declares `adapters: [{family, library, sha256}]`: `family` is the contributed adapter family ID, `library` is a package-relative shared-library path, and `sha256` is its 64-character SHA-256 hex digest. IsOS verifies the package and passes each exact resolved library as `--adapter-library <family>=<absolute-path>@<sha256>`. EOS rehashes the opened inode and loads that inode, checks the `aether_adapter_v1` descriptor's ABI version, family, and supported kind, then creates exactly one instance per node. Libraries are held until their instances are destroyed. MuJoCo keeps its process/socket boundary, and `mock-cxx` remains compiled in.

The deployed EOS runtime starts one adapter host process per package hardware node. Only that host maps the package library. The parent keeps dispatcher admission and sends version-1 bounded JSON requests over an owner-only Unix socket: `heartbeat`, `call` (the C ABI operation ID and typed payload), and `stop`. Every request returns `{ok,result}` or `{ok:false,error}`; camera replies are capped at 64 MiB and all other replies at 1 MiB. A host missing heartbeats for 500 ms invokes the adapter's independent `stop` even if another call is blocked, then exits. The parent treats a lost host or IPC timeout as unavailable and never retries a movement command automatically. A physical controller must still enforce its own lease if the host itself dies. This IPC is a control and observation seam, not a 100 Hz bus-cycle transport: the driver runs its bus loop locally using its latest admitted target.

| Host IPC field | Type | Purpose and validation |
| --- | --- | --- |
| `version` | `u32` | Required request schema version; only `1` is accepted. |
| `op` | `"heartbeat" \| "call" \| "stop"` | Required operation; no implicit fallback. |
| `operation` | `u32` | Required for `call`; C ABI operation `1–7`. |
| `request` | typed JSON value | Required for `call`; matches the C ABI operation payload. |
| `ok` | `bool` | Required response success flag. |
| `result` | typed JSON value | Present on success; matches the C ABI result. |
| `error` | `String` | Present on failure; human-readable rejection reason. |

The public C header is [`include/aether/adapter_v1.h`](../../../include/aether/adapter_v1.h). ABI version is `1` (unsigned integer); `family` is a NUL-terminated UTF-8 ID; `kind_mask` uses camera `1`, base `2`, manipulator `4`; `create` receives `{path, kind, node}` JSON (canonical path, derived kind, validated node profile) and returns an owned instance; `call` uses operation IDs `1–7` for status, health, camera capture, base observe/move, and manipulator observe/move; `stop` must work concurrently with a blocked call and latch before returning; `destroy` releases the instance. Input and output buffers are caller-owned and bounded to 64 MiB for camera capture and 1 MiB for other outputs. Nonzero `call` status returns a UTF-8 error. Typed JSON payloads follow the structs below; move calls receive `{meta, movement}` and return one of `accepted`, `completed`, `cancelled`, or `ambiguous`. A physical adapter or controller enforces independent bounded stop and movement leases.

## Camera RGB and registered depth

| Struct.field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `CameraFrame.meta` | `SampleMeta` | — | Provenance, frame, and freshness of the RGB/depth pair. |
| `CameraFrame.width_px` | `u32` | px | Image width; greater than zero. |
| `CameraFrame.height_px` | `u32` | px | Image height; greater than zero. |
| `CameraFrame.stride_bytes` | `u32` | bytes/row | RGB row stride; at least `3 × width_px`. |
| `CameraFrame.rgb8` | `Vec<u8>` | bytes | Interleaved RGB8 image; length exactly `stride_bytes × height_px`. |
| `CameraFrame.depth` | `Option<DepthFrame>` | — | Registered depth for the same frame when supported; explicit absence otherwise. |
| `CameraFrame.preview_jpeg` | `Option<CameraJpeg>` | — | Optional encoded preview of the same RGB image; absent for adapters without a JPEG source. |
| `CameraJpeg.bytes` | `Vec<u8>` | bytes | JPEG preview, 4 bytes to 8 MiB, with JPEG start and end markers; included in the camera payload bound. |
| `CameraJpeg.quality` | `u8` | JPEG quality | Source encoder quality, 1–100; matching Vision subscriptions may reuse the bytes at full width. |
| `DepthFrame.depth_u16` | `Vec<u16>` | scaled depth values | One depth value per RGB pixel; length exactly `width_px × height_px`. |
| `DepthFrame.depth_scale_m` | `f64` | m/value | Converts a raw depth value to metres; finite and greater than zero. |
| `DepthFrame.intrinsics` | `Intrinsics` | — | Calibration for interpreting aligned depth in the camera frame. |
| `Intrinsics.fx` | `f64` | px | Horizontal focal length; finite and greater than zero. |
| `Intrinsics.fy` | `f64` | px | Vertical focal length; finite and greater than zero. |
| `Intrinsics.cx` | `f64` | px | Principal point horizontal coordinate; finite. |
| `Intrinsics.cy` | `f64` | px | Principal point vertical coordinate; finite. |
| `Intrinsics.distortion_model` | `String` | — | Names the calibration model; nonempty. |
| `Intrinsics.distortion_coefficients` | `Vec<f64>` | model-specific | Ordered calibration coefficients; each finite. |

`CameraFrame::validate(max_payload_bytes)` checks dimensions, checked buffer lengths, calibration, alignment, and the combined RGB, depth, and preview byte limit. The source timestamp, clock, and frame ID describe the pair as one observation.

## Base observation and movement

| Struct.field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `BaseObservation.meta` | `SampleMeta` | — | Provenance and freshness for measured state. |
| `BaseObservation.valid` | `bool` | — | Whether pose and velocity are usable; false must block state-dependent commands. |
| `BaseObservation.x_m` | `f64` | m | Measured planar x in `meta.frame_id`; finite when valid. |
| `BaseObservation.y_m` | `f64` | m | Measured planar y; finite when valid. |
| `BaseObservation.yaw_rad` | `f64` | rad | Measured planar orientation; finite when valid. |
| `BaseObservation.x_m_s` | `f64` | m/s | Measured x velocity; finite when valid. |
| `BaseObservation.y_m_s` | `f64` | m/s | Measured y velocity; finite when valid. |
| `BaseObservation.yaw_rad_s` | `f64` | rad/s | Measured yaw rate; finite when valid. |
| `BaseMovement::BodyVelocity.x_m_s` | `f64` | m/s | Body-frame x velocity; finite and within configured linear limit. |
| `BaseMovement::BodyVelocity.y_m_s` | `f64` | m/s | Body-frame y velocity; finite and within configured linear limit. |
| `BaseMovement::BodyVelocity.yaw_rad_s` | `f64` | rad/s | Body-frame yaw rate; finite and within angular limit. |
| `BaseMovement::BodyVelocity.valid_for_ms` | `u64` | ms | Short lease after which a production controller must stop; greater than zero, at most `max_velocity_lease_ms`, and within the remaining command deadline. The CXX mock reports zero on reads after expiry but has no physical watchdog. |
| `BaseMovement::RelativePose.x_m` | `f64` | m | Relative x displacement accepted by the device controller; finite and within translation limit. |
| `BaseMovement::RelativePose.y_m` | `f64` | m | Relative y displacement; finite and within translation limit. |
| `BaseMovement::RelativePose.yaw_rad` | `f64` | rad | Relative yaw; finite and within rotation limit. |
| `BaseMovement::RelativePose.frame_id` | `String` | coordinate frame | Frame for the relative target; must equal `node.frame_id`. No implicit transform. |

`body_velocity` and `relative_pose` are separate advertised modes. A relative target is a controller-level command; the HAL does not build a route or navigate.

## Manipulator observation and movement

| Struct.field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `ManipulatorObservation.meta` | `SampleMeta` | — | Provenance and freshness for joint and tool state. |
| `ManipulatorObservation.joint_names` | `Vec<String>` | — | Ordered joint identity; nonempty, unique names. |
| `ManipulatorObservation.joint_positions_rad` | `Vec<f64>` | rad | Measured complete joint positions in name order; matching length, finite when valid. |
| `ManipulatorObservation.joint_velocities_rad_s` | `Vec<f64>` | rad/s | Measured joint velocities in the same order; matching length, finite when valid. |
| `ManipulatorObservation.joints_valid` | `bool` | — | Whether the joint arrays can be used for state-dependent admission. |
| `ManipulatorObservation.end_effector_pose` | `Option<EndEffectorPose>` | — | Measured tool pose if reported; explicit absence otherwise. |
| `ManipulatorObservation.end_effector_valid` | `bool` | — | Whether the reported tool pose is usable. |
| `EndEffectorPose.position_m` | `[f64; 3]` | m | XYZ tool position; every component finite. |
| `EndEffectorPose.orientation_xyzw` | `[f64; 4]` | unit quaternion | XYZW orientation; finite and approximately unit length. |
| `EndEffectorPose.frame_id` | `String` | coordinate frame | Frame of tool pose; required nonblank. |
| `ManipulatorMovement::JointPositions.joint_names` | `Vec<String>` | — | Ordered target names; must exactly match the adapter's complete joint set. |
| `ManipulatorMovement::JointPositions.joint_positions_rad` | `Vec<f64>` | rad | Complete target array in name order; matching length, finite, within limits. |
| `ManipulatorMovement::JointPositions.max_speed_rad_s` | `Option<f64>` | rad/s | Optional caller ceiling; finite, greater than zero, within configured limit. |
| `ManipulatorMovement::EndEffectorPose.pose` | `EndEffectorPose` | m and quaternion | Controller-handled Cartesian target; its `frame_id` must equal `node.frame_id`; no implicit transform or Rust IK. |
| `ManipulatorMovement::EndEffectorPose.max_speed_m_s` | `Option<f64>` | m/s | Optional caller ceiling; finite, greater than zero, within configured limit. |

The dispatcher rejects a mode not declared by the profile. The C++ adapter or device controller is responsible for kinematics, trajectories, collision protection, and physical limit enforcement.

## Dispatcher request and discovery

These types live in `aether-dispatcher`. Admission is in-process; `Dispatcher::new_durable` stores command identity and outcomes across restarts, while `Dispatcher::new` uses an in-memory journal for tests.

| Struct.field | Rust type | Unit | Purpose and validation |
| --- | --- | --- | --- |
| `CommandRequest.node_path` | `String` | canonical node path | Selects one admitted hardware node by its full profile path; unknown or software paths are rejected. Local node ID is only for adapter registration. |
| `CommandRequest.meta` | `CommandMeta` | — | Correlation and deadline described above. |
| `CommandRequest.movement` | `Movement` | — | `Base(BaseMovement) | Manipulator(ManipulatorMovement)`; variant must match node kind. |
| `DispatchResult.command_id` | `String` | — | Echoes the requested correlation ID for result lookup. |
| `DispatchResult.status` | `OutcomeStatus` | — | `Completed | Cancelled | Rejected | InProgress | OutcomeUnknown`; uncertain physical execution stays terminal unknown. |
| `DispatchResult.reason` | `Option<String>` | — | Human diagnostic for rejection or uncertainty; absent on confirmed success. |
| `NodeDescriptor.path` | `String` | registry path | Full canonical `<core>/<layer>/<group>/<id>` address for hardware discovery and commands. |
| `NodeDescriptor.node_id` | `String` | — | Stable local ID used to match an adapter registration to its profile node. |
| `NodeDescriptor.kind` | `NodeKind` | — | Camera, base, or manipulator classification for admitted hardware nodes. |
| `NodeDescriptor.label` | `String` | — | Human display label from profile. |
| `NodeDescriptor.facets` | `Vec<NodeFacet>` | — | Available `Status | Health | Observation | Movement | Safety` interfaces. |
| `NodeDescriptor.capabilities` | `Vec<String>` | — | Advertised capabilities, distinct from live health. |

`Dispatcher::new(&Profile, Vec<AdapterRegistration>, Arc<dyn Clock>)` requires exactly one kind- and adapter-type-matching registration per hardware node. `discover()` returns admitted hardware descriptors; `capture_camera(node_path)` returns a validated, fresh camera frame; `dispatch(CommandRequest)` returns a result; `result(command_id)` queries the selected journal; `stop_node(node_path)`, `cancel`, and `emergency_stop` use the safety seam. `Runtime::from_file` in `aether-runtime` reads at most 1 MiB of profile data, validates it, and composes the dispatcher. This runtime is in-process foundation code and is not yet the deployed `eosd` entry point.

## Registry verification

Unit and stubbed graph tests cover known and unknown node types, version and placement rejection, typed port mismatches, JSON/TOML round trips, IsOS process settings and dependency cycles, and custom actuator safety registration. On 2026-10-30 China time, `cargo test -p aether-node-profile -p aether-dispatcher` passed 8 profile and 25 dispatcher tests, `cargo test -p aether-runtime --lib` passed 4 runtime tests, and `cargo clippy -p aether-node-profile -p aether-dispatcher --all-targets -- -D warnings` passed. Complete local output is in `logs/test/20261029T180750Z/node-profile.log`.

## 0.x catalog slice

For a newly authored graph, `version` is `0.1.0` and `node.node_type` selects an `@v0.1` registration. The existing `node.description` remains an instance-specific explanation; new instances must provide nonblank text, while imported old instances may retain their existing empty value until edited. A registered type has its own nonblank `description` explaining function and operator-visible behavior. The registry keeps old `@v1.0` definitions for old graphs and supplies a safe fallback description when an old sidecar omits the new field. Type IDs are never silently rewritten in saved graphs.

EOS registers new built-in identities `eos.hardware.camera@v0.1`, `eos.hardware.base@v0.1`, `eos.hardware.manipulator@v0.1`, `eos.dispatcher.control@v0.1`, `eos.dispatcher.primitive_base@v0.1`, `eos.dispatcher.primitive_manipulator@v0.1`, and `eos.interface.camera_gate@v0.1`. They retain their family placements, kinds, lifecycle, capabilities, and typed port names; their complete descriptions and setting metadata are published in the catalog. The dispatcher control node represents the one shared admission owner in a new graph, including the IsOS starter graph; primitive bindings still connect to hardware through that owner's governed route. The old `eos.control.manual@v1.0`, `eos.control.primitive@v1.0`, `eos.control.skill@v1.0`, and `eos.process.backend@v1.0` do not gain `@v0.1` aliases. The MuJoCo package contributes `eos.process.mujoco@v0.1`; EGIA and IsOS own their new type descriptors. EOS does not redefine those descriptors when IsOS aggregates the catalog.

The target registry extends each `Setting` without changing its existing `name`, `data_type`, or `required` meaning:

| Field | Type and unit | Purpose, default, validation, compatibility |
| --- | --- | --- |
| `description` | optional nonblank string, — | Operator help for the setting; required in new `@v0.1` definitions, absent old definitions use the setting name as fallback. |
| `unit` | optional string, declared physical unit | Display and documentation unit; absent means no physical unit, not a conversion instruction. |
| `min`, `max` | optional signed 64-bit integer, setting unit | Inclusive integer bounds for integer settings; values are nonnegative and `min <= max`. Compare exactly after data-type parsing. Existing settings without bounds keep their current validation. |
| `choices` | optional array of unique strings, — | Static admitted values; empty or absent means no static restriction. The current value remains visible even if unavailable after an upgrade, but Apply rejects it. |
| `selector` | optional object, — | Dynamic option source described in [node contributions](node-contributions.md#target-setting-selector); absent means normal editor input. It cannot authorize an unverified package path. |

Bounds apply only to integer setting types and must be integral, nonnegative, and within the declared type's range; choices apply only to string settings. A setting cannot combine a static choice restriction with a dynamic selector. A selector's provider, resource, and package source are validated at registration, and its selected value is validated by IsOS against the current verified options before Apply. Port descriptions and applicable transport/data-class metadata are included on new built-ins; `expected_rate_hz` remains absent unless an owner declares a measured rate. Catalog metadata never changes exact port data-type identity or authorizes a command.

An EOS reader first parses the graph version, loads exact package sidecars, validates type and instance descriptions under that version's rules, and checks all port/data types before constructing adapters. The new fields are optional at decoding so old sidecars and profiles remain readable. An old executable that lacks support for `0.1.0` is rejected by IsOS preflight before launch; unknown future versions fail rather than being guessed. The published gRPC field numbers, ProtoJSON names, HTTP routes, adapter ABI, and MuJoCo socket version are unchanged by this graph migration.

**G05 setup reader contract.** `aether-profile setup --json` identity retains `graph_version:"1.0.0"` and `node_type_version:"1.0"` as legacy scalar aliases and adds `supported_graph_versions:["1.0.0","0.1.0"]` and `supported_node_type_versions:["1.0","0.1"]`. The latter describes EOS-owned type identities, not permission to accept arbitrary IDs with those suffixes. Its `0.1.0` Validate/Render path projects the `eos` core from the complete IsOS graph, validates that core against exact EOS built-ins and verified package sidecars, and emits a revision-specific `eos/graph.json` projection for the runtime. IsOS launches `aether-runtime` and `aether-config-server` with that verified generated path for `0.1.0`, never the saved full-graph path. IsOS validates its process core and every cross-core edge/reference before migration and Run; until G06 admission is available, it rejects all cross-core edges/references. EGIA validates its agent core. The `1.0.0` path retains its existing whole-profile behavior for compatibility. Unknown EOS types and wrong package provenance fail before an adapter is constructed. EOS does not invent IsOS or EGIA descriptors. The runtime, profile tool and config server also advertise `graph-0.1-reader` in their capabilities. The [P05 reader-features contract](reader-features.md) adds actual selected-binary and required-feature preflight before migration, Apply, Run, or Restart; that cross-repository check remains target work.

The G04 fixture set serializes all seven new EOS built-ins and the MuJoCo process sidecar, checks every type's placement, kind, lifecycle, capabilities, settings, port identity and metadata, and exercises the existing catalog routes. Negative fixtures cover duplicate IDs, blank descriptions, invalid bounds or choices, malformed selectors, unknown codecs and wrong package provenance. Compatibility fixtures keep all old built-ins and a `@v1.0` sidecar without new metadata readable without modifying its bytes. Graph fixtures cover a new starter with its dispatcher, a generic pinned MuJoCo device, and one primitive binding per actuator kind; bad route, target, placement and package cases fail before adapter construction.

G04 verification: `cargo fmt --all -- --check`, `cargo clippy -p aether-node-profile -p aether-runtime --all-targets -- -D warnings`, and `cargo test -p aether-node-profile -p aether-runtime` passed on 2026-11-01 China time. Full local output is in `logs/test/20261101-g04-node-catalog/`. The live MuJoCo test remains simulator-gated and was ignored by the focused command.

## R03 Dispatcher admission target contract

**R03 qualified on merged EOS and IsOS owners.** The [gRPC owner contract](grpc.md#r03-governed-actuator-admission-target-contract) defines the external revision, grant carriers and errors. This section defines the in-process Dispatcher boundary. A managed actuator command has an immutable `AdmissionContext` with `active_revision: String` (IsOS saved whole-graph SHA-256), `authority_role: EgiaControl | OperatorControl` (validated role, not caller text), `ingress: Grpc | PrivateBridge` (server-attested channel, not caller text), `binding_path: String` (canonical registered primitive node path), and `primitive: String` plus `primitive_version: String` (exact admitted catalog identity). `CommandRequest.meta.command_id` remains a 1–128 byte caller ID and `deadline_monotonic_ns` remains an absolute local monotonic deadline. `node_path` remains the exact hardware target derived from the binding, never accepted as an independent client-selected destination. These fields are required for a managed movement; old in-process `dispatch` callers are local compatibility/test helpers and cannot become production ingress. The graph edge is a binding declaration, not a command.

The one Dispatcher admission entry checks the immutable context against its active graph generation at the final release gate after any queue wait. It rejects a missing or changed saved revision, unknown role, absent or multiply targeted binding, wrong primitive/mode or adapter kind, unhealthy or stale node health, invalid or stale required observation, nonfinite values, configured limit or frame/unit mismatch, expired deadline, emergency latch and movement inhibit before calling an adapter. Health and observation freshness use `Clock::monotonic_ns()` and profile `stale_after_ms`, never source wall time. A failed sample degrades that node until a fresh valid probe; it does not grant another route. A stop operation has its own durable kind. Either managed role may request `safety.estop` without a current revision, but must authenticate; internal shutdown uses its private safety seam. Evidence routes and their caps cannot admit or silently drop movement or stop.

A durable record's canonical identity includes command ID, IsOS deployment run ID, stable role/scope and ingress, saved active revision, primitive/version, binding path, exact target and typed arguments. Credential bytes, transport request ID and per-attempt deadline are excluded from the signature; a new attempt must still satisfy its live deadline before release. The journal syncs Prepared and Releasing before physical release and stores terminal or unknown result. Same-ID identical identity returns its retained result; same-ID changed identity rejects without release. A recovered Releasing command is `OutcomeUnknown`, movement remains inhibited until a physically proven explicit recovery, and no automatic retry occurs. A write failure before release calls no adapter; one after possible release cannot become a success claim. Existing historical journal entries lacking the new identity cannot be reinterpreted under the new graph. The [R03 fixture](../../../tests/fixtures/r03_admission_revision/README.md) freezes fake-clock, durable restart and bypass tests.

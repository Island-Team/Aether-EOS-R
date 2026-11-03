# EOS package node contributions

[中文](../../../docs_cn/eos/interfaces/node-contributions_cn.md) · [Unified hardware profile](hardware.md) · [Primitive bindings](primitive-bindings.md)

IsOS installs or links Aether packages and owns the applied graph. A package release may contribute registered node types and node sets; IsOS verifies the release and writes a derived, read-only node-type sidecar for EOS. EOS reads the graph and sidecar without changing either. This JSON interface is the EOS-specific node IDL; it describes identities, placement, settings, and typed ports, while hardware samples and commands remain Rust values inside EOS. The example below is the **current** `@v1.0` format; type description is implemented, while the other target extensions remain planned.

## Verified node-type sidecar

IsOS passes each verified sidecar as a repeatable `--node-types <absolute-path>` argument to `aether-profile`, `aether-runtime`, and `aether-config-server`. Each file is at most 1 MiB. EOS loads all safety declarations before the corresponding types, rejects duplicate or malformed definitions, and validates the graph against the combined built-in and contributed registry. All enum values use `snake_case`; unknown fields fail decoding.

```json
{
  "package": {"id": "island/mujoco-sim", "version": "0.1.1"},
  "safety_adapters": [{"id": "mujoco-base-stop", "kind": "base", "movement_modes": ["body_velocity"], "supports_stop": true}],
  "data_types": [],
  "adapter_families": [{"id":"mujoco-v2","device_kinds":["camera","base","manipulator"],"transport":"unix_socket","description":"MuJoCo device socket adapter","supported_primitives":["base.velocity@1.0.0","arm.movej@1.0.0"]}],
  "node_types": [{
    "id": "eos.hardware.mujoco_base@v1.0", "core": "eos", "layer": "hardware", "group": "mujoco",
    "kind": "base", "lifecycle": "adapter", "capabilities": ["body_velocity"],
    "settings": [], "inputs": [{"name": "command", "data_type": "base_movement", "required": true}],
    "outputs": [{"name": "observation", "data_type": "base_observation", "required": false}],
    "safety_adapter_type": "mujoco-base-stop"
  }]
}
```

`package` is an exact `{id, version}` reference. The ID has two valid address segments separated by `/`; version has three nonempty numeric components. A contributed node instance must carry the same `node.package` value as the type's sidecar. Built-in nodes omit `node.package`. Registration checks a declared stop capability, but EOS admits a physical actuator only when a compiled adapter and stop implementation exist for its type and package.

`node_types` is an array of `NodeType` definitions from [the hardware contract](hardware.md#node-type-registry-and-typed-connections); `safety_adapters` is an array of `SafetyAdapterType`. Their Rust and JSON fields are identical. `core`, `layer`, and `group` are required placement segments; the graph cannot infer hardware kind from a group. The `kind` values are `camera`, `base`, `manipulator`, and `software`; lifecycle values are `internal`, `process`, and `adapter`. IsOS package node sets use these type IDs and symbolic graph references, but the applied graph contains only concrete paths.

`data_types` is an optional array of `DataTypeDescriptor` entries. Each entry has `id` (string, unique versioned identifier such as `vendor.camera.depth@v1.0`), `version` (string, `1.0`, matching the ID suffix), `purpose` (nonempty description string), `media_type` (nonempty MIME string), `codec` (nonempty registered runtime codec ID), `schema` (JSON object describing logical fields, including `description`, `x-unit`, and JSON Schema validation keywords), and `max_payload_bytes` (positive integer, bytes). Built-in IDs remain snake case, such as `camera_frame` and `vision_frame`. A contributed port can reference a data type only when its codec is compiled into EOS. `json_v1` and `vision_frame_jpeg_v1` are built-in codecs. EOS exposes the combined catalog through `aether-profile catalog` and `GET /api/eos/v2/data-types` on the config server.

An edge requires the exact same registered `data_type` ID at its source and input. Its schema's `x-unit` annotations describe payload fields for display and validation within that type; they do not make two different type IDs interchangeable. No implicit unit conversion occurs on a graph edge. A contributed descriptor cannot redefine a registered ID with different units; duplicate IDs fail registration. IsOS owns edge-addressed Apply diagnostics, while EOS owns runtime health after launch. The [hardware edge contract](hardware.md#g06-typed-edge-admission) defines source QoS, route classification, transport and safety restrictions. A package-contributed port cannot claim a command route merely by setting `data_class: "command"` or matching a built-in movement type: a hardware command input accepts only the registered Dispatcher primitive-binding source.

`adapter_families` is an optional array. Each descriptor requires `id` (valid family segment), `device_kinds` (nonempty list of `camera`, `base`, or `manipulator`), `transport` (one of `in_process`, `unix_socket`, `websocket`, or `ros2`), and `description` (nonblank string). Optional `settings_schema` is a JSON object; optional `supported_primitives` is an array of registered `<name>@<version>` strings. The sidecar's outer `package` supplies exact provenance. Package authors cannot set `compiled`; EOS derives it from its compiled adapter factory and exposes `compiled` and `reason` in the catalog. Generic hardware instances carry this package in `node.adapter.package`, independently of the built-in node type.

Each input or output port requires `name` (graph-local string), `data_type` (registered ID string), and `required` (boolean). Optional `description` explains its role; `expected_rate_hz` is a finite positive number in hertz; `transport` is one of `in_process`, `unix_socket`, `websocket`, or `ros2`; `data_class` is one of `image`, `observation`, `command`, `event`, `state`, or `opaque`; `cardinality` is `one` or `many` (default `one`). The edge validator uses cardinality, transport and data class; `expected_rate_hz` remains a display hint, not an edge cap. A new `@v0.1` type supplies transport and data class for every port. Legacy omitted transport means `in_process`; omitted data class is inferred from the registered built-in type or treated as non-routable. Runtime data identity still uses the exact registered `data_type` ID. See the [Vision interface](vision.md) for the JPEG codec and read-only transport.

## MuJoCo contribution

The MuJoCo package currently contributes `eos.process.mujoco@v1.0` at `eos/process/mujoco` and the `mujoco-v2` adapter family. Its new-graph sidecar contributes `eos.process.mujoco@v0.1` with a nonblank type description and complete setting and port metadata; it does not contribute a new MuJoCo-specific hardware type. Its node set uses generic built-in camera, base, and manipulator `@v0.1` types at `eos/hardware/<kind>/<id>` with `node.adapter.package = {id = "island/mujoco-sim", version = "0.1.1"}`. The process node carries `node.package` because its type is contributed. Its settings are `scene` and `socket`, with optional `env` and `start_timeout_ms`; package identity is not duplicated in settings. Each device adapter references that process node. Base velocity and arm joint-position bindings link to device command inputs; the shared EOS Dispatcher still admits every actuator command.

The legacy built-in `eos.process.backend@v1.0` and hardware types remain valid for existing graphs. Their process settings retain `package` and `version` until those graphs migrate. New package types use the package reference above.

## Typed data identity

EOS may wrap an in-process hardware value in `NodeDataEnvelope<T>`. The envelope carries `source_node` (canonical graph path), `port` (registered output name), `data_type` (`PortDataType`), `contract_version` (`2.0.0`), and the typed `payload`. `NodeDataEnvelope::output` rejects an output port whose declared type differs from `T::DATA_TYPE`. The envelope is metadata for identifying data against the IsOS graph; it does not grant command admission or create a second actuator path.

`TypedDataHub` publishes such envelopes to registered output keys. `subscribe<T>` validates the node's output port and returns a typed cursor; `poll_latest` returns only a newer value. The hub retains one value per output, so a slow subscriber skips intermediate values without a growing queue. Dropping the cursor disconnects that reader. A compiled `DataTransformer<Input, Output>` names its codec ID and accepts typed options; `CameraJpegTransformer` converts `camera_frame` to `vision_frame` with `vision_frame_jpeg_v1`.

Invalid sidecar JSON, duplicate type IDs, missing safety adapters, package mismatches, unknown types, placement mismatches, and incompatible port edges fail validation before runtime construction. Missing compiled actuator implementations fail runtime adapter construction before the EOS socket binds.

## 0.x type description — implemented slice

New package-contributed `@v0.1` node types require a nonblank `description` string that explains the node's function, inputs, outputs, and operator-visible effect. It appears in the EOS catalog and will appear in the planned IsosUI add-node selector. Built-in types have the same field. An old `@v1.0` sidecar without `description` remains readable and receives a generated catalog fallback without changing its verified bytes. An empty description on a new type, duplicate type ID, mismatched exact package pin, or unsupported codec fails registration before Apply. The outer `package` and adapter-family `description` retain their existing meanings.

## Target setting selector

`Setting.selector` is an optional JSON object on new type definitions. It describes where IsOS obtains options; it never embeds executable code or an arbitrary filesystem path. Existing `name`, `data_type`, `required`, `description`, `unit`, bounds, and static choices retain the rules in [hardware](hardware.md#0x-catalog-slice).

| Field | JSON type and unit | Purpose, default, validation, compatibility |
| --- | --- | --- |
| `provider` | string enum, — | Required when `selector` exists: `package`, `registry`, or `graph`; absent `selector` keeps the ordinary editor. |
| `resource` | nonblank string ID, — | Required option family, such as `scene`, `device`, `adapter_family`, or `node`; resolved only by the selected provider. Unknown families fail registration. |
| `package_from` | optional string enum, — | Required for `provider=package`: `node.package` or `node.adapter.package`; forbidden otherwise. It selects the exact verified pin in the draft graph. |

```json
{"name":"scene","data_type":"string","required":true,"description":"Verified MuJoCo scene ID","selector":{"provider":"package","resource":"scene","package_from":"node.package"}}
```

IsOS recomputes options from the draft graph and installed, verified release each time the dependency changes. A scene or device selection stores its stable ID in node settings, never the resolved absolute path. IsOS resolves the asset and checks its digest again at Apply and run. A selected ID that is no longer installed stays visible in the editor with `available=false` but fails Apply. The [IsOS node-options contract](https://github.com/Island-Team/isos/blob/master/docs/isos/interfaces/local-api.md) owns the response envelope and error shape. This metadata supports future model-weight options without enabling `egia.tensor` in this release.

## R04 structured setting metadata — target design, not implemented

R04 adds the reusable `Setting.data_type = "object_array"` value and optional `items_schema`, `min_items`, `max_items` fields. Existing settings serialize unchanged when these fields are absent. `min_items` and `max_items` are nonnegative integer entry counts, with `min_items <= max_items <= 16`; they apply only to `object_array`. `items_schema` is required for that type, is an object of at most 8 KiB and depth at most three, and supports only local `type`, `properties`, `required`, `additionalProperties`, `minLength`, `maxLength` and `pattern` keywords. Unknown keywords, `$ref`, malformed nested schema, wrong item type or count fail registration or graph validation before Apply. The generic registry validates shape and bounds; the owning graph validator applies semantic checks.

IsOS owns its new `isos.process.component@v0.1` descriptor and its `settings.skill_libraries` item schema. EOS does not publish that IsOS type. Its [R04 host ABI contract](skills/native/host-abi.md) gives the concrete object item and selected-file handoff; IsOS applies package pin and path checks. A truthful object-array descriptor lets IsosUI render the setting without encoding structured package selection as a misleading string. A graph setting is still a declaration, never a command or worker grant.

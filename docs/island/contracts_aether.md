# Aether EOS contracts

[中文](../../docs_cn/island/contracts_aether_cn.md) · [EOS contract architecture](../eos/develop/interfaces/contracts.md) · [Conventions](conventions.md)

**Island** is the team, **Aether** is the product version, and **EOS** owns these public interfaces. The authored source is `share/contracts/eos/`; its short path does not determine the wire package. Keep `island.eos.v1` as the stable protobuf package. The historical `egia.contracts.*` namespace remains in schema IDs; the latest payloads now use v1 IDs.

## Source, artifacts, and ownership

Author public Gateway, Skill, Workflow, system, robot, perception, and primitive interfaces in focused `.proto` files under `share/contracts/eos/`. Keep the three service entry points at the EOS contract root: `gateway.proto`, `skills.proto`, and `workflow.proto`. Their request and response messages live in the matching domain folders; for example, `skills.proto` defines the RPCs while `skills/execution.proto` defines execution payloads. The other folders are `system/` for resource and fact messages, `robot/` for descriptors, navigation, and primitive bindings, and `perception/` for camera and perception payloads. `options/` holds shared JSON Schema annotations and `external/http/` holds HTTP route annotations. `actions/`, `body/`, and `world/` define the generated runtime JSON Schema families. See the [source map](../eos/develop/interfaces/contracts.md#source-map) for file locations. Vendored Google API annotations live under `share/contracts/google/`. Use optional fields for independently absent payloads and `oneof` for mutually exclusive alternatives. Private model-provider schemas and gRPC service definitions live under `share/contracts/eos/external/models/`; the simulator protocol lives under `external/simulator/`. The native southbound HAL has no public JSON Schema.

The generator compiles proto descriptors and derives runtime JSON Schemas and `contracts.manifest.json` under `share/ext/eos/`: `actions/` contains primitive calls and acknowledgements, `body/` contains body validation payloads, and `world/` contains perception and status resources. Generated JSON is reviewable output, never an editing source. The optional north HTTP gateway derives `share/ext/north_http/openapi.json` from proto annotations, including HTTP-only and streaming routes. `EOS_NORTH_HTTP` controls whether the gateway and OpenAPI file are built and installed; `north-http.toml` holds listener settings. CMake generates C++ bindings in the build tree. Island-egia maintains any language-specific mirror in its own repository.

## Public boundary

EOS exposes `GatewayService`, `SkillService`, and `WorkflowService` through native gRPC. EGIA initializes with transport health and valid `Discover` and `ListResources` responses. `Discover.devices[].primitives` is the primitive discovery surface. Each exact `(primitive, version, device_id)` binding supplies kind, admitted reference modes, bounded numeric limits, health, dependencies, accepted media types, and optional absolute frame. EGIA and EOS validate against that binding before hardware release. Northbound has no device or primitive registration API; compact robot and device state are typed `Discover` and `GetStatus` fields.

An action request uses the selected generated schema for its arguments:

```json
{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

The public contract exposes only the latest v1 shape for each interface. Action objects reject unknown fields and allow optional `type` and a legacy nullable `command_id` alias. Callers supply the top-level `CallPrimitiveRequest.command_id`; EOS injects the matching value before schema validation. A missing version is invalid. JSON Schema defaults are annotations, not inserted values. Values use metres, radians, newtons, and milliseconds as appropriate. See [primitive calls](../eos/develop/interfaces/actions.md) for the exact payloads and discovery rules.

Every dispatch returns a terminal `primitive.ack` with a non-null result status. Command IDs are reserved before release; identical retries replay the original response and conflicting reuse never actuates. Stable rejections use `accepted: false` in a successful RPC, while malformed input and unknown primitive identities use transport errors. Device primitives require a generated device ID; process-wide stops require an empty target.

`SkillService` provides searchable descriptors and asynchronous execution, including execution snapshots, deadlines, replay, signals, journals, and cancellation. A skill may run only after its arguments pass the returned schema. Installation is an operator action; clients cannot register executable paths. North HTTP maps the asynchronous API to REST and SSE. See [skill authoring](../eos/develop/interfaces/skills/native/README.md).

EOS defines discriminator-backed external world, arm state, raw RGB vision, semantic detection, compact VAD, and compact ASR payloads. The external-world schema has no current readable route. Detection excludes masks, keypoints, and tensors; compact VAD and ASR exclude audio and provider data. Raw RGB is available through the bounded `CaptureFrames` snapshot interface. Resource payloads are checked against the generated schemas; an unknown ID is not a compatibility alias.

## Changing a contract

Keep existing RPC names, field numbers, ProtoJSON names, and public discriminators compatible. This latest-only reset replaces former v2 schema IDs and primitive versions with v1; clients must update their exact IDs and accepted payloads together with EOS. For later incompatible changes, replace the public interface and update clients atomically, retaining only the newest active shape. Regenerate outputs and check freshness from the repository root:

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/ext/eos
python3 scripts/generate_eos_contracts.py --output share/ext/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north_http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts "$PWD/share"
python3 tests/eos/test_contract_manifest.py "$PWD/share"
python3 tests/eos/test_contract_interfaces.py "$PWD/share"
```

Review descriptor and ProtoJSON compatibility, schema acceptance, route coverage, C++ consumers, and both HTTP build settings. Update the [EOS interface guide](../eos/develop/interfaces/contracts.md) and relevant tests with each public change. `share/skills/native/` remains source for reviewed product skills; deployment-specific binaries and mutable state remain outside `share/`.

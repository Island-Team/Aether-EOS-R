# Aether EOS contracts

**Unified source target (unreleased):** EOS remains the wire-contract owner under `share/eos/`; EI in `ei/` consumes generated bindings from that in-repository source. New graph types are `isos.<layer>.<name>@v0.2` inside one `isos` core. Published protobuf packages and historical `egia.contracts.*` IDs remain compatibility identities. The detailed C++ surface below is historical.

<!-- docs-tree:start -->
**Location:** [Docs](../README.md) / [Island](README.md) / **contracts aether**

<details>
<summary>Docs tree</summary>

- [docs/](../README.md)
  - [island/](README.md)
    - [interfaces/](interfaces/README.md)
    - **contracts_aether.md**
    - [conventions.v1.3.1.md](conventions.v1.3.1.md)
    - [layout.md](layout.md)

</details>
<!-- docs-tree:end -->

[中文](../../docs_cn/island/contracts_aether_cn.md) · [EOS contract architecture](../eos/interfaces/contracts.md) · [Conventions](conventions.v1.3.1.md)

**Scope:** this page inventories published contract source and the opt-in C++ service surface. The 0.x target retains compatible Gateway and Capability identities for the planned Rust gRPC gateway; it does **not** add WorkflowService, ActionList, or a graph-authored skill sequence. Existing `workflow.proto` and related protobuf names remain historical compatibility identities. Follow the [active EOS interface catalog](../eos/interfaces/README.md) and [historical catalog](../eos/legacy/README.md) to distinguish target behavior from preserved source.

**Island** is the team, **Aether** is the product version, and **EOS** owns these public interfaces. The authored source is `share/eos/contracts/eos/`; its short path does not determine the wire package. Keep `island.eos.v1` for Gateway, Skill, and Workflow; `CapabilityService` uses `island.eos.v2`. The historical `egia.contracts.*` namespace remains in schema IDs; the latest payloads now use v1 IDs.

## Source, artifacts, and ownership

Author public Gateway, Skill, Workflow, system, robot, perception, and primitive interfaces in focused `.proto` files under `share/eos/contracts/eos/`. Keep the four service entry points at the EOS contract root: `gateway.proto`, `skills.proto`, `workflow.proto`, and `capabilities.proto`. Their request and response messages live in the matching domain folders; for example, `skills.proto` defines the RPCs while `skills/execution.proto` defines execution payloads. The other folders are `system/` for resource and fact messages, `robot/` for descriptors, navigation, and primitive bindings, and `perception/` for camera and perception payloads. `options/` holds shared JSON Schema annotations and `external/http/` holds HTTP route annotations. `actions/`, `body/`, and `world/` define the generated runtime JSON Schema families. See the [source map](../eos/interfaces/contracts.md#source-map) for file locations. Vendored Google API annotations live under `share/eos/contracts/google/`. Use optional fields for independently absent payloads and `oneof` for mutually exclusive alternatives. Private model-provider schemas and gRPC service definitions live under `share/eos/contracts/eos/external/models/`; the simulator protocol lives under `external/simulator/`. The native southbound HAL has no public JSON Schema.

The generator compiles proto descriptors and derives runtime JSON Schemas and `contracts.manifest.json` under `share/eos/schemas/eos/`: `actions/` contains primitive calls and acknowledgements, `body/` contains body validation payloads, and `world/` contains perception and status resources. Generated JSON is reviewable output, never an editing source. The optional north HTTP gateway derives `share/ext/north-http/openapi/north-http/openapi.json` from proto annotations, including HTTP-only and streaming routes. `EOS_NORTH_HTTP` controls whether the gateway and OpenAPI file are built and installed; `north-http.toml` holds listener settings. CMake generates C++ bindings in the build tree. Island-egia maintains any language-specific mirror in its own repository.

## Public boundary

The opt-in C++ EOS service surface exposes `GatewayService`, `SkillService`, `WorkflowService`, and `CapabilityService` through native gRPC. The default Rust process does not yet serve this gateway. EGIA initializes with transport health and valid `Discover` and `ListResources` responses. `Discover.devices[].primitives` is the primitive discovery surface. Each exact `(primitive, version, device_id)` binding supplies kind, admitted reference modes, bounded numeric limits, health, dependencies, accepted media types, and optional absolute frame. Agents select the corresponding canonical name from `ListCapabilities`; EOS validates the resolved binding before hardware release. Northbound has no device or primitive registration API; compact robot and device state are typed `Discover` and `GetStatus` fields.

An action request uses the selected generated schema for its arguments:

```json
{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","operationId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

EOS 0.2.0 retires the public v1 Gateway primitive action RPCs. `CapabilityService.SubmitOperation` takes a canonical `capabilityName`, an `operationId`, and an object `arguments`. The selected action schema rejects unknown fields; EOS injects its internal `type` and `command_id` before validation. JSON Schema defaults are annotations, not inserted values. Values use metres, radians, newtons, and milliseconds as appropriate. See [primitive calls](../eos/interfaces/actions.md) for the exact payloads and discovery rules.

A terminal operation includes a `primitive.ack` with a non-null result status. Command IDs are reserved before release; identical retries replay the original response and conflicting reuse never actuates. Stable rejections use `accepted: false` in a successful RPC, while malformed input and unknown primitive identities use transport errors. Device capabilities resolve an admitted device; process-wide stop capabilities use the `runtime/{robot_id}/...` namespace.

The published C++ `SkillService` provides searchable descriptors and asynchronous execution, including execution snapshots, deadlines, replay, signals, journals, and cancellation. A skill may run only after its arguments pass the returned schema. Installation is an operator action; clients cannot register executable paths. North HTTP maps the asynchronous API to REST and SSE. See [skill authoring](../eos/interfaces/skills/native/README.md).

EOS defines discriminator-backed external world, arm state, raw RGB vision, semantic detection, compact VAD, and compact ASR payloads. The external-world schema has no current readable route. Detection excludes masks, keypoints, and tensors; compact VAD and ASR exclude audio and provider data. Raw RGB is available through the bounded `CaptureFrames` snapshot interface. Resource payloads are checked against the generated schemas; an unknown ID is not a compatibility alias.

## Changing a contract

Keep surviving RPC names, field numbers, ProtoJSON names, and public discriminators compatible. The earlier EOS 0.2.0 cutover removed five public v1 Gateway primitive action RPCs and changed some exact schema/primitive IDs; its specific client migration is in the [migration guide](../eos/interfaces/capability-migration.md). Future interface and node-profile extensions follow the [0.x compatibility rule](conventions.v1.3.1.md#testing-and-observability): add optional fields with defaults, keep old readers' supported wire shape where possible, and make new readers accept old data. An incompatible change gets a new contract identity and explicit migration; do not silently replace IDs. Regenerate outputs and check freshness from the repository root:

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north-http/openapi/north-http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts "$PWD/share"
python3 tests/eos/test_contract_manifest.py "$PWD/share"
python3 tests/eos/test_contract_interfaces.py "$PWD/share"
```

Review descriptor and ProtoJSON compatibility, schema acceptance, route coverage, C++ consumers, and both HTTP build settings. Update the [EOS interface guide](../eos/interfaces/contracts.md) and relevant tests with each public change. Deployment-specific skills and mutable state remain local and ignored by Git.

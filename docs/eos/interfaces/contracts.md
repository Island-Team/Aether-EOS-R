# EOS contract architecture

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Interfaces](README.md) / **contracts**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [interfaces/](README.md)
      - [external/](external/README.md)
      - [skills/](skills/README.md)
      - [southbound/](southbound/README.md)
      - [actions.md](actions.md)
      - [body.md](body.md)
      - [capabilities.md](capabilities.md)
      - **contracts.md**
      - [grpc.md](grpc.md)
      - [capability-migration.md](capability-migration.md)
      - [skills.md](skills.md)
      - [workflow.md](workflow.md)
      - [world.md](world.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/interfaces/contracts_cn.md) · [Interface overview](README.md) · [Aether contract rules](../../island/contracts_aether.md)

The public EOS contract source is `share/eos/contracts/eos/`. Four files at that root, `gateway.proto`, `skills.proto`, `workflow.proto`, and `capabilities.proto`, define the process-facing gRPC services and their HTTP route annotations. Domain folders hold their messages. Gateway, Skill, and Workflow remain in `island.eos.v1`; CapabilityService uses `island.eos.v2`.

## Source map

| Location under `share/eos/contracts/eos/` | Contents |
| --- | --- |
| `gateway.proto`, `skills.proto`, `workflow.proto` | Service RPCs and HTTP bindings |
| `system/` | Resources and facts |
| `robot/` | Robot descriptors, navigation, and primitive bindings |
| `perception/` | Camera, media, faces, humans, pose, roles, VAD, and monitoring |
| `skills/` | Catalog, execution, journal, signal, and shared skill messages |
| `workflow/` | Action, state, and template messages |
| `options/` | Shared JSON Schema annotations |
| `actions/`, `body/`, `world/` | Authored proto sources for generated runtime JSON Schemas |
| `external/http/` | HTTP route annotations for the adapter |
| `external/models/` | Private model-provider gRPC contract and capability JSON Schemas |
| `external/simulator/` | Private simulator gRPC socket contract |

For example, edit `skills.proto` when changing a SkillService RPC and `skills/execution.proto` when changing its execution payload. These folders organize source files; they do not introduce new protobuf packages or schema ID namespaces.

## Service message sources

### System resources and facts

System messages cover model status, resource history, change events, retention, and ActionList facts. Gateway and Workflow services use [`system/resource.proto`](../../../share/eos/contracts/eos/system/resource.proto) and [`system/fact.proto`](../../../share/eos/contracts/eos/system/fact.proto); their RPC behavior is documented in the [gRPC](grpc.md) and [workflow](workflow.md) guides.

### Robot discovery and commands

Robot messages define discovery descriptors, admitted primitive bindings and call requests, and named navigation points. Gateway owns the RPCs. See [`robot/descriptor.proto`](../../../share/eos/contracts/eos/robot/descriptor.proto), [`robot/primitive.proto`](../../../share/eos/contracts/eos/robot/primitive.proto), and [`robot/navigation.proto`](../../../share/eos/contracts/eos/robot/navigation.proto). The [body](body.md) and [actions](actions.md) guides explain validation and device identity.

### Perception requests and media

Perception messages cover camera capture, object observation, VAD, people, pose, faces, roles, media, and live monitor events. Gateway exposes the calls; the [world guide](world.md) explains projected resources. Sources are [`camera.proto`](../../../share/eos/contracts/eos/perception/camera.proto), [`media.proto`](../../../share/eos/contracts/eos/perception/media.proto), [`vad.proto`](../../../share/eos/contracts/eos/perception/vad.proto), [`humans.proto`](../../../share/eos/contracts/eos/perception/humans.proto), [`pose.proto`](../../../share/eos/contracts/eos/perception/pose.proto), [`faces.proto`](../../../share/eos/contracts/eos/perception/faces.proto), [`roles.proto`](../../../share/eos/contracts/eos/perception/roles.proto), and [`monitor.proto`](../../../share/eos/contracts/eos/perception/monitor.proto).

### Contract annotations

The [`external/http/http.proto`](../../../share/eos/contracts/eos/external/http/http.proto) and [`options/schema.proto`](../../../share/eos/contracts/eos/options/schema.proto) extensions carry HTTP route metadata and embedded JSON Schema source. They guide generation and validation; they do not create an EOS service or transport. See the [HTTP adapter contract](external/http.md) for route behavior.

## Generated JSON Schemas

Proto options describe JSON validation that protobuf types cannot express. The generator derives runtime JSON Schemas and the manifest under `share/eos/schemas/eos/`:

- `actions/`: versioned primitive call schemas and `primitive.ack`.
- `body/`: seven device-class validation payloads.
- `world/`: perception, obstacle, and status resource payloads.

The generated manifest records roles, paths, schema IDs, and digests. EOS loads and verifies it at startup. The historical `egia.contracts.*` ID namespace remains, and each versioned payload has its own schema ID. Typed `Discover.devices[]` and status fields are protobuf payloads. The `body/*.proto` schemas define public body shapes used to validate EOS projections; they do not introduce additional readable resources.

When the external north HTTP package is enabled with `EOS_NORTH_HTTP`, the same proto definitions generate `share/ext/north-http/openapi/north-http/openapi.json`. The adapter translates REST/SSE and WebSocket routes to the EOS gRPC services. Listener settings remain in `north-http.toml`.

A capability submission illustrates the boundary: protobuf owns the outer envelope, and the selected generated action schema validates `arguments`.

```json
{"capabilityName":"hardware/demo/base/default/base.velocity@1.0.0","operationId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

For behavior, see [body](body.md), [world](world.md), [primitive calls](actions.md), and [HTTP/SSE](external/http.md). Private model-provider request and response schemas live under `share/eos/contracts/eos/external/models/schemas/`; the native hardware HAL has no shared JSON Schema.

## Updating a contract

Edit a focused proto module, regenerate derived JSON, and review changes to the generated schemas and OpenAPI. Keep surviving RPC names, protobuf field numbers, ProtoJSON keys, and public discriminators compatible. EOS 0.2.0 deliberately retires the five public v1 Gateway primitive action RPCs; see the [migration guide](capability-migration.md). The latest-only v1 reset replaces former v2 schema IDs and primitive versions, so clients must update their exact identifiers and accepted payloads with EOS.

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos
python3 scripts/generate_eos_contracts.py --output share/eos/schemas/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north-http/openapi/north-http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts /absolute/path/to/share
```

Run contract, schema, and gateway tests, including a build with `EOS_NORTH_HTTP` enabled when HTTP routes change. Private model-provider payloads are outside this public generation flow.

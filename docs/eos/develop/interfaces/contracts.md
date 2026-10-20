# EOS contract architecture

[中文](../../../../docs_cn/eos/develop/interfaces/contracts_cn.md) · [Interface overview](README.md) · [Aether contract rules](../../../island/contracts_aether.md)

The public EOS contract source is `share/contracts/eos/`. Three files at that root, `gateway.proto`, `skills.proto`, and `workflow.proto`, define the process-facing gRPC services and their HTTP route annotations. Domain folders hold their messages. The wire package remains `island.eos.v1` regardless of source path.

## Source map

| Location under `share/contracts/eos/` | Contents |
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

System messages cover model status, resource history, change events, retention, and ActionList facts. Gateway and Workflow services use [`system/resource.proto`](../../../../share/contracts/eos/system/resource.proto) and [`system/fact.proto`](../../../../share/contracts/eos/system/fact.proto); their RPC behavior is documented in the [gRPC](grpc.md) and [workflow](workflow.md) guides.

### Robot discovery and commands

Robot messages define discovery descriptors, admitted primitive bindings and call requests, and named navigation points. Gateway owns the RPCs. See [`robot/descriptor.proto`](../../../../share/contracts/eos/robot/descriptor.proto), [`robot/primitive.proto`](../../../../share/contracts/eos/robot/primitive.proto), and [`robot/navigation.proto`](../../../../share/contracts/eos/robot/navigation.proto). The [body](body.md) and [actions](actions.md) guides explain validation and device identity.

### Perception requests and media

Perception messages cover camera capture, object observation, VAD, people, pose, faces, roles, media, and live monitor events. Gateway exposes the calls; the [world guide](world.md) explains projected resources. Sources are [`camera.proto`](../../../../share/contracts/eos/perception/camera.proto), [`media.proto`](../../../../share/contracts/eos/perception/media.proto), [`vad.proto`](../../../../share/contracts/eos/perception/vad.proto), [`humans.proto`](../../../../share/contracts/eos/perception/humans.proto), [`pose.proto`](../../../../share/contracts/eos/perception/pose.proto), [`faces.proto`](../../../../share/contracts/eos/perception/faces.proto), [`roles.proto`](../../../../share/contracts/eos/perception/roles.proto), and [`monitor.proto`](../../../../share/contracts/eos/perception/monitor.proto).

### Contract annotations

The [`external/http/http.proto`](../../../../share/contracts/eos/external/http/http.proto) and [`options/schema.proto`](../../../../share/contracts/eos/options/schema.proto) extensions carry HTTP route metadata and embedded JSON Schema source. They guide generation and validation; they do not create an EOS service or transport. See the [HTTP adapter contract](external/http.md) for route behavior.

## Generated JSON Schemas

Proto options describe JSON validation that protobuf types cannot express. The generator derives runtime JSON Schemas and the manifest under `share/ext/eos/`:

- `actions/`: fifteen primitive calls and `primitive.ack`.
- `body/`: seven device-class validation payloads.
- `world/`: nine perception and status resource payloads.

The generated manifest records roles, paths, schema IDs, and digests. EOS loads and verifies it at startup. The historical `egia.contracts.*` ID namespace remains, and each active payload now has one v1 ID. Typed `Discover.devices[]` and status fields are protobuf payloads. The `body/*.proto` schemas define public body shapes used to validate EOS projections; they do not introduce additional readable resources.

When the external north HTTP package is enabled with `EOS_NORTH_HTTP`, the same proto definitions generate `share/ext/north_http/openapi.json`. The adapter translates REST/SSE and WebSocket routes to the EOS gRPC services. Listener settings remain in `north-http.toml`.

A primitive call illustrates the boundary: protobuf owns the outer envelope, and the selected generated action schema validates `arguments`.

```json
{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

For behavior, see [body](body.md), [world](world.md), [primitive calls](actions.md), and [HTTP/SSE](external/http.md). Private model-provider request and response schemas live under `share/contracts/eos/external/models/schemas/`; the native hardware HAL has no shared JSON Schema.

## Updating a contract

Edit a focused proto module, regenerate derived JSON, and review changes to the generated schemas and OpenAPI. Keep existing RPC names, protobuf field numbers, ProtoJSON keys, and public discriminators compatible. The latest-only v1 reset replaces former v2 schema IDs and primitive versions, so clients must update their exact identifiers and accepted payloads with EOS.

```sh
uv sync --only-group contracts
python3 scripts/generate_eos_contracts.py --output share/ext/eos
python3 scripts/generate_eos_contracts.py --output share/ext/eos --check
python3 scripts/generate_north_http_openapi.py --output share/ext/north_http/openapi.json
python3 scripts/generate_north_http_openapi.py --check
./scripts/print_contract_interfaces.py --contracts /absolute/path/to/share
```

Run contract, schema, and gateway tests, including a build with `EOS_NORTH_HTTP` enabled when HTTP routes change. Private model-provider payloads are outside this public generation flow.

# gRPC process interface

[Interface guide](README.md) · [中文](../../../../docs_cn/eos/develop/interfaces/grpc_cn.md)

## Transport ownership

The EOS process-facing protobuf services are `GatewayService`, `SkillService`, and `WorkflowService`. EOS exposes all three on one gRPC listener. A separate `eos-north-http` process exposes their annotated REST operations and server-streaming SSE operations:

- native gRPC on `[gateway].grpc_port`, owned by `eosd`;
- HTTP/1.1 and SSE on `[north_http].port`, owned by `eos-north-http`.

The north HTTP process calls only the public gRPC services; it does not link or bypass into EOS domain services. Typed protobuf envelope fields use their ProtoJSON names, such as `deviceId`, `frameCount`, and `absoluteFrame`. Schema-owned values carried in `google.protobuf.Struct` retain their documented JSON keys, including `schema_version` and primitive argument snake_case names. ProtoJSON base64-encodes protobuf `bytes`, including camera JPEG pictures.

The domain pages document the native protobuf/gRPC services. The HTTP page documents strict ProtoJSON routes and the SSE projections of `WatchResource`, `WatchExecution`, `FollowExecutionJournal`, and `MonitorExecutions`.

Native clients use package `island.eos.v1` and the generated contracts from `share/contracts/eos/`. Unary and streaming calls accept optional `x-request-id` metadata for correlation. Clients must set deadlines, cancel streams they no longer consume, preserve `uint64` sequence/revision values, and branch on gRPC status codes rather than diagnostic message text. Domain rejection and terminal failure are normally successful RPC responses carrying typed status fields; malformed, missing, conflicting, or unavailable requests use canonical gRPC statuses documented by each service page. Workflow clients bootstrap ActionList/fact state with `GetRuntimeSnapshot.action_list_cursor`, then resume the manager-wide `WatchActionListEvents` stream with the last durably applied `{epoch, sequence}`. Epoch mismatch, future cursor, and retention expiry require a new snapshot as documented by the [WorkflowService recovery algorithm](workflow.md#resnapshot-and-resume-algorithm). The standalone simulator retains its separately documented JSON and binary formats. The gateway binds loopback TCP by default and may also bind an optional owner-controlled Unix domain socket as documented by the [northbound runtime](../../design/runtime/northbound.md). The inherited IPC descriptor used by supervised external skills is a separate private interface and is not the EOS gRPC listener.

See the [north HTTP contract](external/http.md) for every route, error mapping, SSE cursor rule, CORS behavior, request ID, and browser security boundary.


## Read, compute, and write admission

The gRPC server is an adapter over EOS-owned domain services. Read RPCs use EOS projections and retained stores: discovery, status, resource/history reads, watches, navigation reads, and skill/workflow queries do not synchronously query a hardware driver. Hardware-backed body data is sampled by EOS on its runtime cadence. A read requiring such a sample fails when it is missing, unhealthy, or more than one second old. Successful body reads expose the age in milliseconds as initial gRPC metadata `eos-sample-age-ms`; the HTTP adapter forwards it as `Eos-Sample-Age-Ms`. This leaves resource JSON schemas unchanged. Reads still check resource identity, freshness, bounds, and response size.

Camera capture and perception inference are bounded compute calls. They may use a validated snapshot and call a model provider; they have deadlines, cancellation, and capacity limits. They are not treated as unrestricted cache reads.

Primitive calls and cancellation, waypoint changes, skill execution and signals, and workflow submissions are writes. Their gRPC handlers call the owning admission path, which checks schemas, capabilities, target, safety policy, idempotency, and queues before state changes. Handlers do not call the hardware HAL directly. A transport request ID correlates logs; operation-specific command and request IDs retain their documented replay rules.

EOS treats clients reaching its configured local listener as trusted under deployment policy. Loopback TCP does not distinguish local users. For same-host clients, prefer the optional owner-controlled Unix socket. Per-client grants require a separate access-control design; there is no permissive security placeholder in the write path.

## Assigned listener ports

EOS-owned listeners are contiguous and must remain so:

| Port | Owner | Protocol |
| --- | --- | --- |
| `16003` | `eosd` | EOS gRPC |
| `16004` | `eos-simulator` | simulator HTTP/WebSocket |
| `16005` | `eos-north-http` | browser REST/SSE |

Ports in model-provider or test fixtures are not EOS listeners and are outside this allocation.

Unknown methods, resources, schemas, versions, fields forbidden by a schema, enum values, targets, and primitive names fail closed. Validation happens before admission or provider work. Empty and optional values retain the meaning defined by their schema; transports must not turn absence into a default unless the contract explicitly defines one. Size, deadline, cancellation, ordering, and degraded-dependency behavior are defined by the focused interface page and the owning runtime module.

See the [contract catalog](contracts.md), the [runtime overview](../../design/runtime/README.md), and the [extension overview](../ext/README.md) for loading, execution, and device-side dataflow.

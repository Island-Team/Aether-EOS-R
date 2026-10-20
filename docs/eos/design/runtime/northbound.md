# Northbound runtime

[Runtime overview](README.md) · [Interface overview](../../develop/interfaces/README.md) · [Workflow adapter](northbound/workflow.md) · [中文](../../../../docs_cn/eos/design/runtime/northbound_cn.md)

`src/eos/runtime/northbound/` serves `island.eos.v1.GatewayService`, `island.eos.v1.SkillService`, and `island.eos.v1.WorkflowService` on one native gRPC listener. EOS core does not embed an HTTP listener. Contracts live in `share/contracts/eos/gateway.proto`, `share/contracts/eos/skills.proto`, and `share/contracts/eos/workflow.proto`.

Northbound is a transport adapter. Perception, Dispatcher, SkillManager, and ActionList remain the authoritative domain owners; protobuf conversion and gRPC lifecycle do not move their state into the transport.

## Module map

| Source folder | Documentation | Responsibility |
| --- | --- | --- |
| `runtime/northbound/` | this page | shared server, Gateway adapter, Skill adapter, codecs, resource subscriptions, health, and reflection |
| `runtime/northbound/workflow/` | [workflow.md](northbound/workflow.md) | `WorkflowService` unary conversion and ActionList event streaming |

The workflow child module has its own `CMakeLists.txt` and contributes its service implementation to `eos_northbound`. Shared request context, duration validation, listener lifecycle, and generated contracts remain at the northbound boundary.

## Services

`GatewayService` provides `Discover`, `GetStatus`, `ListResources`, `ReadResource`, `QueryResourceHistory`, `WatchResource`, `CallPrimitive`, `CancelPrimitive`, `CaptureFrames`, and the typed unary `InferVad`, `DetectHumans`, `EstimateHumanPose`, and `DetectFaces` calls. It also provides typed `ListNavigationPoints`, `GetNavigationPoint`, `PutNavigationPoint`, `DeleteNavigationPoint`, and `ReloadNavigationPoints` operations for the core-private navigation-point store. Navigation-point mutations are serialized by the store; incomplete coordinates are rejected, and reload is the explicit synchronization boundary after an operator edits the JSON snapshot directly.

Registered product histories take precedence for list/read/watch operations; `QueryResourceHistory` supports exact sequence, relative timestamp, and bounded inclusive sequence-range selectors with explicit retention-gap metadata. History values and their boot, epoch, sequence, and time metadata have the same privacy boundary as current resource reads; authorization remains a deployment concern rather than a second history-specific framework. Resources without a registered history, including cheap body device states and stored navigation points, continue through a resource provider or the legacy body reader without acquiring a model producer. Stored points appear as `robots/{robot_name}/navigation/waypoints/{name}` and project an absolute map target containing `frame_id`, `x_m`, `y_m`, and `yaw_rad`. Each inference resolves the exact requested model ID and capability before calling the models runtime, converts protobuf bytes to the canonical model contract's base64 field, and cooperatively forwards gRPC cancellation. Face registration is not part of this inference surface. Perception, the navigation-point store, and Dispatcher own the underlying data and action semantics.

`SkillService` provides catalog discovery, process-like execution management, SIGNAL admission, lifecycle replay, and unified lifecycle/outbound-SIGNAL event watches. SkillManager owns exact runtime routing, validation, queues, claims, events, retention, and execution. Disconnecting a skill stream does not cancel its execution.

Skill runtimes do not receive a storage handle. Built-in and native skills read stored navigation points through the same read-only resource capability used for perception resources. Native skills must declare `robots/{robot_name}/navigation/waypoints/{waypoint_id}` before reading it; `navigate_waypoint@1.0.0` uses that declaration when no inline target is supplied. Skills cannot list the backing table, mutate points, delete points, reload the file, or subscribe to point history. Those operations remain Gateway/operator capabilities.

`WorkflowService` provides ActionList apply/get/list, atomic ActionList/fact cursor snapshots, and manager-wide ActionList/fact event replay. Its focused implementation boundary is documented in the [workflow adapter](northbound/workflow.md), and its wire contract and recovery algorithm are documented in [WorkflowService](../../develop/interfaces/workflow.md).

The external north HTTP process uses one shared generated Workflow stub for browser routes and its optional non-authoritative audit projection. Neither consumer is linked into EOS core, and audit persistence never changes this service's authoritative state.

## Workflow replay

`GetRuntimeSnapshot` returns independently sequenced skill executions plus ActionLists, facts, and `action_list_cursor` copied atomically by the ActionList manager. `WatchActionListEvents` replays complete ActionList revisions and accepted facts strictly after a required `{epoch, sequence}` cursor, then follows live mutations.

The core retains 4,096 workflow events by default. Epoch mismatch or a same-epoch future sequence maps to `FAILED_PRECONDITION`; a cursor older than the retained window maps to `OUT_OF_RANGE`; a missing or empty cursor maps to `INVALID_ARGUMENT`. These statuses instruct the client to obtain a fresh snapshot and resume. Northbound does not repair, skip, merge, or synthesize domain events.

## Cross-cutting transport behavior

Every inbound RPC runs in the standard request-correlation scope. `StartExecution` and ActionList target dispatch preserve correlation identity onto asynchronous work. Request cancellation ends server work that belongs to the call or stream; it does not implicitly cancel an accepted skill execution, primitive command, or ActionList.

The server enables standard gRPC health and reflection for all three services. Malformed transport input maps to `INVALID_ARGUMENT`, missing identities to `NOT_FOUND`, idempotency conflicts to `ALREADY_EXISTS`, exhausted bounded storage to `RESOURCE_EXHAUSTED`, state/cursor epoch and model-capability conflicts to `FAILED_PRECONDITION`, expired workflow cursors to `OUT_OF_RANGE`, missing composed services to `UNAVAILABLE`, and invalid model responses to `DATA_LOSS`. Primitive, skill, and workflow semantic outcomes remain typed successful payloads after acceptance.

## Logging and debugging

The `northbound` logger records service, method, final numeric gRPC status, duration, correlation identity, stream open/close where implemented, and bounded transport failure reasons. It never logs arguments, results, schemas, facts, ActionLists, event payloads, cursor values, resource payloads, primitive acknowledgements, credentials, or raw protobuf.

Normal unchanged resource samples, empty event-follow waits, and polling loops remain silent. Correlate `northbound` request completion with the owning `dispatch`, `skills`, or `actionlist` domain logger. For workflow recovery, the client must retain the returned gRPC status and its last applied cursor because cursor values are intentionally not duplicated into server logs.

## Lifecycle and configuration

`NorthboundServer::Run()` always binds the configured TCP listener and optionally binds `unix:<grpc_uds_path>` on the same gRPC server, then starts the tick thread, reports ready, and blocks in gRPC serving. UDS startup requires an absolute normalized path beneath existing non-symlink directories and a process-owned immediate parent with mode `0700`. The private parent closes gRPC's create-then-chmod exposure window without changing the process-global `umask`; consequently, group/world bits on `grpc_uds_mode` cannot make the socket reachable through that parent. Startup refuses to replace a non-socket or a socket with a live listener and unlinks only a same-inode socket whose connection probe returned `ECONNREFUSED`. It applies owner-only `0o600` permissions by default and removes only the socket inode created by this server during shutdown. `Stop()` shuts down gRPC and joins the tick thread before referenced domain services are destroyed.

```sh
./bin/eosd \
  --contracts /path/to/share \
  --skills-dir /path/to/native-skills \
  --host 127.0.0.1 \
  --grpc-port 16003
```

The TCP address comes from `[gateway] host` and `grpc_port`; command-line flags override it. The default is loopback. Optional `grpc_uds_path` adds a UDS listener and `grpc_uds_mode` configures its permission bits. The listener provides neither authentication nor TLS, so deployments must not bind it to an untrusted interface or grant socket access to untrusted users without an external security boundary.

## Testing

Generated-stub integration tests cover all three services, standard health/reflection, resource stream cancellation, skill replay/cancellation, Workflow lifecycle and snapshot conversion, live `WatchActionListEvents`, and workflow epoch rejection. ActionList unit tests cover cursor expiry and snapshot atomicity below the adapter.

Contract-interface tests verify service and field descriptors from the protobuf sources. A lifecycle test reserves the legacy HTTP port while starting EOS to ensure the core opens only its configured gRPC listener.

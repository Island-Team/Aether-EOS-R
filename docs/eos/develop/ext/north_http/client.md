# North HTTP gRPC client

[North HTTP process](README.md) · [Server](server.md) · [Audit](audit.md) · [Native northbound](../../../design/runtime/northbound.md) · [中文](../../../../../docs_cn/eos/develop/ext/north_http/client_cn.md)

`src/eos/ext/north_http/client/` owns the process-wide gRPC connectivity from `eos-north-http` to `eosd`. Its public type is `eos::ext::north_http::Client`.

## Ownership boundary

`Client(std::string upstream)` creates and owns exactly one insecure `grpc::Channel` for the configured EOS target. From that channel it creates exactly one generated `GatewayService::Stub`, one `SkillService::Stub`, and one `WorkflowService::Stub`.

The client owns no HTTP routing, ProtoJSON conversion, deadlines, request metadata, retries, readiness state, or domain semantics. Those operations belong to the server, gRPC library, or EOS service reached by the call.

## Channel and stub set

`channel() const` returns a const reference to the shared channel pointer. `gateway()`, `skills()`, and `workflows()` return references to the owned stubs. Callers borrow these objects and must not delete or replace them.

All unary calls, browser streams, audit snapshot/stream calls, and generic gRPC health probes use this same channel. The channel admits responses up to 64 MiB so typed model responses, including an explicitly requested annotated face image, are not constrained by gRPC's smaller default receive limit. The fixed stub set mirrors the three services exposed on the single EOS listener; the module does not create a channel per request, component, or service.

Channel construction is non-blocking and does not require EOS to be ready. gRPC manages connection and reconnection beneath the stubs. The client does not add an application retry loop and does not convert gRPC outcomes.

## Lifetime and use

`Client` is non-copyable. The process constructs it before optional `audit::Service` and `Server`, injects it by reference into both, and destroys it only after the server and audit have stopped and released every call context and worker.

The stubs may be used by concurrent server and audit workers through their generated synchronous methods. Each operation still creates its own `grpc::ClientContext`; browser request metadata/deadlines/cancellation belong to Server, audit snapshot/stream context and reconnect policy belong to the audit service, and neither policy lives on the shared channel.

## Logging and debugging

The client module emits no logs and does not inspect payloads. Logger `north-http.client` has a reserved routed file but currently owns no events or request-context policy. Connection-state transitions become observable when the server or audit paths report them through their owning loggers. Diagnose channel reachability with `/health/ready`, then correlate browser failures by request ID or audit recovery by cursor and stable status.

The channel uses `grpc::InsecureChannelCredentials`; it supplies neither TLS nor authentication. Keep the configured EOS endpoint inside the trusted local boundary unless another deployment layer provides transport security.

## Testing

`eos_north_http_client_tests` verifies one non-null stable channel and stable accessors for all three generated stubs. Server integration tests exercise health, unary calls, and browser streams through the injected client; audit tests exercise Workflow snapshot and event resume through the same client.

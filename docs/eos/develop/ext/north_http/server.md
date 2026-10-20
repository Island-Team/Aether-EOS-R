# North HTTP server

[North HTTP process](README.md) · [gRPC client](client.md) · [Audit](audit.md) · [Configuration](config.md) · [REST/SSE contract](../../interfaces/external/http.md) · [中文](../../../../../docs_cn/eos/develop/ext/north_http/server_cn.md)

`src/eos/ext/north_http/server/` owns the HTTP/1.1, SSE, and live-monitor WebSocket adapter implemented by `eos::ext::north_http::Server`. The public wire contract remains [north HTTP and WebUI](../../interfaces/external/http.md); this page defines implementation ownership and debugging boundaries.

## Ownership boundary

`Server(Config, Client&, audit::Service* = nullptr)` copies its admitted configuration, borrows the process-wide `Client`, and optionally borrows the process-owned audit service. It owns the listener, bounded connection-worker set, separate unary/SSE/monitor admission counters, active HTTP streams, registered per-call `grpc::ClientContext` objects, readiness observations, route mapping, ProtoJSON conversion, CORS, SSE framing, and HTTP/gRPC status conversion.

Both injected objects must outlive the server. The server does not create channels or service stubs, write the audit journal, or own EOS domain state. Every EOS-dependent route calls the public Gateway, Skill, or Workflow stub from the shared client; audit routes read synchronized projections through `audit::Service`.

## Requests and upstream calls

`/health/live` is local. `/health/ready` performs a bounded generic `grpc.health.v1.Health/Check` over `Client::channel()`. Optional audit health is reported separately by `/v1/eos/audit/status` and never gates the functional HTTP adapter. REST routes parse strict ProtoJSON, enforce path/query authority, prepare a fresh call context with the request correlation metadata and configured deadline, invoke the matching stub, and encode the protobuf response as ProtoJSON.

The server maps canonical gRPC statuses to the documented HTTP codes and returns the stable JSON error envelope. It does not reinterpret accepted primitive, skill, or ActionList outcomes as transport failures and does not retry semantic or transport failures.

Navigation points use typed routes under `/v1/eos/robots/{robot_name}/navigation/waypoints`: `GET` lists or reads, `PUT` upserts an item, and `DELETE` removes one. The path owns the robot and point name. `POST /v1/eos/navigation/waypoints:reload` explicitly imports a direct JSON edit. CORS advertises `PUT` and `DELETE` for these routes.

`GET /v1/eos/audit/status` and `GET /v1/eos/audit/action-lists` are local read-only projections with `Cache-Control: no-store`, including invalid-limit and oversized-page errors; disabled audit returns stable empty/disabled shapes. The record route defaults to the newest 100 retained records and admits `limit` in `1..1000`. It returns `{enabled, records, truncated}`, selecting a contiguous newest suffix under a fixed 4 MiB serialized-record-array budget and copying it oldest-first. If the newest record alone exceeds the budget, the route returns HTTP `429` / `RESOURCE_EXHAUSTED` with stable message `newest audit record exceeds response byte limit`.

## Bounds, streams, and shutdown

`max_connections` is the hard bound on retained accepted-connection workers and is also used as the listen backlog. The accept loop reaps completed workers and does not accept another socket while the worker set is full; pending connections therefore remain in the kernel backlog rather than creating more workers. A worker slot is reused only after its completed thread is joined. Configuration requires the three request-class capacities to sum to no more than this worker bound.

Within those workers, non-stream HTTP work, SSE routes, and the live-monitor WebSocket independently consume `max_inflight_requests`, `max_sse_streams`, and `max_monitor_streams`. Occupancy in one class does not consume another class's capacity. The worker first reads the HTTP header, resolves the request ID, classifies the request, and attempts that class's admission before reading any body. If the class is full, it returns HTTP `429` / gRPC `RESOURCE_EXHAUSTED` with the class-specific stable message and common response headers, reports zero request-body bytes, closes the connection, and never reads or dispatches the body. An admitted request retains its class permit through the complete unary response or stream lifetime. Bodies admitted past that gate remain subject to `max_request_bytes`.

SSE writes directly to the peer under the request lifetime, sends comment heartbeats at `sse_heartbeat_ms`, and does not create an unbounded intermediate event queue. Its heartbeat and peer-disconnect helpers contain all exceptions, attempt to cancel the associated gRPC context, actively close the transport when needed to unblock each other, and are stopped and joined before the connection worker returns. The monitor peer-disconnect helper has the same exception containment and context-cancellation rule.

Every upstream operation owns a `grpc::ClientContext`. Active contexts are registered so peer disconnect, failed stream writes, request deadline, and process shutdown can cancel the associated gRPC work. Closing an HTTP/SSE connection cancels that transport call; it does not implicitly cancel an already accepted asynchronous skill execution or workflow.

`GET /v1/eos/workflows:watch` parses `epoch:sequence` from authoritative `cursor` query input or fallback `Last-Event-ID`, calls `WorkflowService.WatchActionListEvents`, and emits `event: action-list-event` with the same `epoch:sequence` as SSE `id`. It follows the same direct-write, heartbeat, cancellation, error-event, and no-unbounded-queue rules as other SSE routes.

`GET /v1/eos/monitor/ws` validates the upgrade origin and one strict subscription, then translates `GatewayService.WatchMonitor`. Control and audio events are JSON text; camera JPEGs use the versioned `EOSM` binary envelope. The handler performs direct writes, cancels gRPC when the peer disconnects, and records only counts, byte totals, duration, and status metadata.

Every connection worker catches all exceptions escaping request processing. It best-effort logs `north_http_worker_error`, closes and unregisters its transport, and uses unconditional completion marking so the accept loop can join the thread and reuse the slot; a request that failed after response processing began may simply observe a closed connection. Helper-thread exceptions likewise cannot terminate the process or strand a joinable helper.

`Run(stop_requested)` blocks after binding and periodically observes the stop callback. Shutdown closes the acceptor, cancels and closes all tracked accepted transports, cancels every registered gRPC context, and joins the worker vector before clearing it. Because acceptance stops at `max_connections`, this shutdown join set is bounded by `max_connections`; sockets still waiting in the listen backlog never become workers or enter the tracked set. `listening()` reports successful bind and `port()` reports the actual listener port, including an ephemeral test port.

## CORS and trust boundary

CORS uses exact origin membership from `allowed_origins`. Remote binding requires the explicit `allow_remote` configuration opt-in, but that flag adds no authentication, authorization, or encryption. A production deployment must keep the listener on loopback or put a trusted security boundary in front of it.

## Logging and debugging

The server owns `north-http` events for listener start/stop, upstream ready/unavailable/reconnected transitions, request rejection/completion, stream closure, and transport errors. Records carry correlation identity and bounded metadata such as method, route, status, duration, byte counts, gRPC code, and cancellation state.

The server never logs request/response bodies, credentials, media, transcript content, skill arguments/results, facts, ActionLists, audit records, cursor values, schemas, event payloads, or raw protobuf. Repeated identical records may be suppressed by the process logging configuration. For an upstream failure, begin with `/health/ready`, then compare the server's gRPC code and correlation ID with `eosd` logs; for audit health, use its status route and `north-http.audit` diagnostics.

## Testing

`eos_north_http_server_tests` runs the server against generated in-process Gateway, Skill, and Workflow fixtures. It covers liveness/readiness, separate degraded-audit readiness, disabled audit routes and `no-store`, Gateway/Skill/Workflow REST routing, strict ProtoJSON rejection, deadline mapping, request IDs, CORS, body bounds, the hard worker bound and slot reuse, independent unary/SSE/monitor admission and header-first `429` responses, worker-exception containment, typed resource/skill/Workflow SSE IDs and resume inputs, invalid Workflow cursor errors, peer-disconnect cancellation, and unavailable upstream behavior. Client, config, and journal behavior remain in their focused suites.

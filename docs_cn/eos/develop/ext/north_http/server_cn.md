# North HTTP server

[North HTTP 进程](README_cn.md) · [gRPC client](client_cn.md) · [Audit](audit_cn.md) · [配置](config_cn.md) · [REST/SSE 契约](../../interfaces/external/http_cn.md) · [English](../../../../../docs/eos/develop/ext/north_http/server.md)

`src/eos/ext/north_http/server/` 拥有由 `eos::ext::north_http::Server` 实现的 HTTP/1.1、SSE 与 live-monitor WebSocket adapter。公开 wire contract 仍为 [north HTTP 与 WebUI](../../interfaces/external/http_cn.md)；本文定义 implementation ownership 与 debugging boundary。

## Ownership boundary

`Server(Config, Client&, audit::Service* = nullptr)` 复制其 admitted configuration，借用 process-wide `Client`，并可选借用 process-owned audit service。它拥有 listener、bounded connection-worker set、独立 unary/SSE/monitor admission counter、active HTTP stream、registered per-call `grpc::ClientContext` object、readiness observation、route mapping、ProtoJSON conversion、CORS、SSE framing 与 HTTP/gRPC status conversion。

两个 injected object 都必须比 server 活得更久。server 不创建 channel 或 service stub、不写 audit journal，也不拥有 EOS domain state。每个 EOS-dependent route 都调用 shared client 中的公开 Gateway、Skill 或 Workflow stub；audit route 通过 `audit::Service` 读取 synchronized projection。

## Request 与 upstream call

`/health/live` 是 local 的。`/health/ready` 通过 `Client::channel()` 执行 bounded generic `grpc.health.v1.Health/Check`。optional audit health 由 `/v1/eos/audit/status` 独立报告，不再 gate functional HTTP adapter。REST route 解析 strict ProtoJSON，强制 path/query authority，使用 request correlation metadata 与 configured deadline 准备 fresh call context，调用 matching stub，并把 protobuf response 编码为 ProtoJSON。

server 把 canonical gRPC status 映射到文档化的 HTTP code，并返回 stable JSON error envelope。它不会把 accepted primitive、skill 或 ActionList outcome 重新解释为 transport failure，也不重试 semantic 或 transport failure。

`GET /v1/eos/audit/status` 与 `GET /v1/eos/audit/action-lists` 是带 `Cache-Control: no-store` 的 local read-only projection，invalid-limit 与 oversized-page error 也带该 header；audit disabled 时返回 stable empty/disabled shape。record route 默认请求最新 100 条 retained record，接受 `1..1000` 内的 `limit`。它返回 `{enabled, records, truncated}`，在固定 4 MiB serialized-record-array budget 内选择 contiguous newest suffix，再以 oldest-first 顺序复制。若 newest record 单独超过 budget，route 返回 HTTP `429` / `RESOURCE_EXHAUSTED` 与 stable message `newest audit record exceeds response byte limit`。

## Bound、stream 与 shutdown

`max_connections` 是 retained accepted-connection worker 的 hard bound，同时也用作 listen backlog。accept loop 回收 completed worker，并在 worker set 已满时不再接受 socket；pending connection 因而留在 kernel backlog，而不会创建更多 worker。只有 completed thread 被 join 后，其 worker slot 才会复用。configuration 要求三个 request-class capacity 的总和不超过该 worker bound。

在这些 worker 内，non-stream HTTP work、SSE route 与 live-monitor WebSocket 分别独立消耗 `max_inflight_requests`、`max_sse_streams` 与 `max_monitor_streams`。一个 class 的 occupancy 不消耗其他 class 的 capacity。worker 先读取 HTTP header、解析 request ID、分类 request 并尝试取得该 class 的 admission，然后才读取任何 body。如果该 class 已满，它返回带 class-specific stable message 与 common response header 的 HTTP `429` / gRPC `RESOURCE_EXHAUSTED`，报告零 request-body byte，关闭 connection，且绝不读取或 dispatch body。admitted request 在完整 unary response 或 stream lifetime 内持续持有其 class permit。通过该 gate 的 body 仍受 `max_request_bytes` 约束。

SSE 在 request lifetime 内直接写入 peer，按 `sse_heartbeat_ms` 发送 comment heartbeat，且不创建 unbounded intermediate event queue。其 heartbeat 与 peer-disconnect helper containment 所有 exception，尝试取消关联 gRPC context，在需要时主动关闭 transport 以相互解除阻塞，并在 connection worker 返回前停止且 join。monitor peer-disconnect helper 遵循相同的 exception containment 与 context-cancellation 规则。

每个 upstream operation 拥有一个 `grpc::ClientContext`。active context 被 registered，以便 peer disconnect、failed stream write、request deadline 与 process shutdown 可取消关联 gRPC work。关闭 HTTP/SSE connection 会取消该 transport call；它不会隐式取消已 accepted asynchronous skill execution 或 workflow。

`GET /v1/eos/workflows:watch` 从 authoritative `cursor` query input 或 fallback `Last-Event-ID` 解析 `epoch:sequence`，调用 `WorkflowService.WatchActionListEvents`，并发出 `event: action-list-event`，同一 `epoch:sequence` 用作 SSE `id`。它遵循与其他 SSE route 相同的 direct-write、heartbeat、cancellation、error-event 与 no-unbounded-queue 规则。

`GET /v1/eos/monitor/ws` 验证 upgrade origin 与一个 strict subscription，然后 translation `GatewayService.WatchMonitor`。control 与 audio event 是 JSON text；camera JPEG 使用 versioned `EOSM` binary envelope。handler 执行 direct write，在 peer disconnect 时取消 gRPC，且仅记录 count、byte total、duration 与 status metadata。

每个 connection worker 捕获所有逃出 request processing 的 exception。它 best-effort 记录 `north_http_worker_error`、关闭并 unregister transport，并使用 unconditional completion marking，使 accept loop 可 join thread 并复用 slot；在 response processing 开始后失败的 request 可能只观察到 connection closed。helper-thread exception 同样不能终止 process 或遗留 joinable helper。

`Run(stop_requested)` 在 bind 后阻塞并定期观察 stop callback。shutdown 关闭 acceptor，取消并关闭所有 tracked accepted transport，取消每个 registered gRPC context，并在清空 worker vector 前 join 它。因为 acceptance 在 `max_connections` 处停止，该 shutdown join set 最多为 `max_connections`；仍在 listen backlog 中等待的 socket 不会成为 worker，也不会进入 tracked set。`listening()` 报告 successful bind，`port()` 报告 actual listener port，包括 ephemeral test port。

## CORS 与 trust boundary

CORS 使用 `allowed_origins` 中的 exact membership。remote binding 需要显式 `allow_remote` configuration opt-in，但该 flag 不添加 authentication、authorization 或 encryption。production deployment 必须把 listener 保持在 loopback，或在其前面放置 trusted security boundary。

## Logging 与调试

server 拥有 listener start/stop、upstream ready/unavailable/reconnected transition、request rejection/completion、stream closure 与 transport error 的 `north-http` event。record 携带 correlation identity 和 bounded metadata，例如 method、route、status、duration、byte count、gRPC code 与 cancellation state。

server 绝不记录 request/response body、credential、media、transcript content、skill argument/result、fact、ActionList、audit record、cursor value、schema、event payload 或 raw protobuf。process logging configuration 可抑制 repeated identical record。对于 upstream failure，先检查 `/health/ready`，再比较 server 的 gRPC code 与 correlation ID 及 `eosd` log；对于 audit health，使用其 status route 与 `north-http.audit` diagnostic。

## 测试

`eos_north_http_server_tests` 针对 generated in-process Gateway、Skill 与 Workflow fixture 运行 server。它覆盖 liveness/readiness、separate degraded-audit readiness、disabled audit route 与 `no-store`、Gateway/Skill/Workflow REST routing、strict ProtoJSON rejection、deadline mapping、request ID、CORS、body bound、hard worker bound 与 slot reuse、独立 unary/SSE/monitor admission 与 header-first `429` response、worker-exception containment、typed resource/skill/Workflow SSE ID 与 resume input、invalid Workflow cursor error、peer-disconnect cancellation 与 unavailable upstream behavior。Client、config 与 journal behavior 保留在它们的 focused suite 中。

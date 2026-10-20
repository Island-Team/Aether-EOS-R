# North HTTP gRPC client

[North HTTP 进程](README_cn.md) · [Server](server_cn.md) · [Audit](audit_cn.md) · [原生 northbound](../../../design/runtime/northbound_cn.md) · [English](../../../../../docs/eos/develop/ext/north_http/client.md)

`src/eos/ext/north_http/client/` 拥有从 `eos-north-http` 到 `eosd` 的 process-wide gRPC connectivity。其公开类型为 `eos::ext::north_http::Client`。

## Ownership boundary

`Client(std::string upstream)` 为 configured EOS target 创建并拥有恰好一个 insecure `grpc::Channel`。它从该 channel 创建恰好一个 generated `GatewayService::Stub`、一个 `SkillService::Stub` 和一个 `WorkflowService::Stub`。

client 不拥有 HTTP routing、ProtoJSON conversion、deadline、request metadata、retry、readiness state 或 domain semantic。这些 operation 属于 server、gRPC library 或 call 到达的 EOS service。

## Channel 与 stub set

`channel() const` 返回 shared channel pointer 的 const reference。`gateway()`、`skills()` 与 `workflows()` 返回 owned stub 的 reference。caller 借用这些 object，不得删除或替换它们。

所有 unary call、browser stream、audit snapshot/stream call 与 generic gRPC health probe 都使用这一个 channel。Channel 接受最大 64 MiB 的 response，因此 typed model response（包括显式请求的 annotated face image）不受 gRPC 较小的默认 receive limit 约束。fixed stub set mirror 单个 EOS listener 公开的三个 service；module 不会为每个 request、component 或 service 创建 channel。

channel construction 不阻塞，也不要求 EOS ready。gRPC 在 stub 下方管理 connection 与 reconnection。client 不添加 application retry loop，也不转换 gRPC outcome。

## Lifetime 与 use

`Client` 不可复制。process 在 optional `audit::Service` 与 `Server` 之前构造它，按 reference 注入两者，并且只在 server 与 audit 停止并释放每个 call context 与 worker 后才销毁它。

stub 可由 concurrent server 与 audit worker 通过其 generated synchronous method 使用。每个 operation 仍创建自己的 `grpc::ClientContext`；browser request metadata/deadline/cancellation 属于 Server，audit snapshot/stream context 与 reconnect policy 属于 audit service，两种 policy 都不属于 shared channel。

## Logging 与调试

client module 不发出 log，也不检查 payload。logger `north-http.client` 有 reserved routed file，但当前不拥有 event 或 request-context policy。当 server 或 audit path 通过其 owning logger 报告 connection-state transition 时，它们才变得可观察。先用 `/health/ready` 诊断 channel reachability，再通过 request ID 关联 browser failure，或通过 cursor 与 stable status 关联 audit recovery。

channel 使用 `grpc::InsecureChannelCredentials`；它不提供 TLS 或 authentication。除非其他 deployment layer 提供 transport security，否则应将 configured EOS endpoint 保持在 trusted local boundary 内。

## 测试

`eos_north_http_client_tests` 验证一个 non-null stable channel，以及三个 generated stub 的 stable accessor。Server integration test 通过 injected client 执行 health、unary call 与 browser stream；audit test 通过同一 client 执行 Workflow snapshot 与 event resume。

# Northbound 运行时

[运行时总览](README_cn.md) · [接口总览](../../interfaces/README_cn.md) · [Workflow adapter](northbound/workflow_cn.md) · [English](../../../../docs/eos/design/runtime/northbound.md)

`adapters/eos-cxx/runtime/northbound/` 在一个原生 gRPC listener 上提供 `island.eos.v1.GatewayService`、`island.eos.v1.SkillService`、`island.eos.v1.WorkflowService` 与 `island.eos.v2.CapabilityService`。EOS core 不嵌入 HTTP listener。契约位于 `share/eos/contracts/eos/`，其中 `capabilities.proto` 定义 [v2 动作表面](../../interfaces/capabilities_cn.md)。

Northbound 是 transport adapter。Perception、Dispatcher、SkillManager 与 ActionList 保持 authoritative domain owner；protobuf conversion 与 gRPC lifecycle 不会把其 state 移入 transport。

v2 CapabilityService 先进入可信的[硬件能力 Broker](kernel_cn.md)，再进入 Dispatcher，列出规范硬件名称，并按能力名称提交一个异步操作。其 `operation_id` 就是 Dispatcher 命令 ID；查询、观察与取消读取该命令记录。重启后恢复的操作没有持久保存的 `capability_name` 字段。此历史设计依赖没有内置调用方认证的现有 listener 部署边界。EOS [R03 受管理准入](../../interfaces/grpc_cn.md#r03-执行器受治理准入目标契约)已为 Rust 执行器写入和操作所有权增加授权与活动修订校验；EOS/IsOS 所有者边界通过合并后 R03 重放。独立 north HTTP 适配器已通过 REST 和 SSE 暴露 v2 路由。五个公开 v1 primitive 动作 RPC 已在 EOS 0.2.0 退役。

## Module map

| Source folder | Documentation | Responsibility |
| --- | --- | --- |
| `runtime/northbound/` | 本文 | shared server、Gateway adapter、Skill adapter、codec、resource subscription、health 与 reflection |
| `runtime/northbound/workflow/` | [workflow_cn.md](northbound/workflow_cn.md) | `WorkflowService` unary conversion 与 ActionList event streaming |

workflow child module 有自己的 `CMakeLists.txt`，并把其 service implementation 加入 `eos_northbound`。shared request context、duration validation、listener lifecycle 与 generated contract 保留在 northbound boundary。

## Services

`GatewayService` 提供 `Discover`、`GetStatus`、`ListResources`、`ReadResource`、`WatchResource`、`CaptureFrames`，以及 typed unary `InferVad`、`DetectHumans`、`EstimateHumanPose` 与 `DetectFaces`。每次 inference 都先解析 request 指定的精确 model ID 与 capability，再调用 models runtime，并把 protobuf bytes 转为 canonical model contract 的 base64 field，同时协作式传递 gRPC cancellation。Face registration 不属于该 inference surface，仍是 primitive。`NorthboundCore` 只拥有 transport cache：bounded two-frame camera cache，以及 `WatchResource` 使用的 sampled-content tracker。Perception 与 Dispatcher 拥有 underlying data 与 action semantic。

`SkillService` 提供 catalog discovery、process-like execution management、SIGNAL admission、lifecycle replay，以及 unified lifecycle/outbound-SIGNAL event watch。SkillManager 拥有 exact runtime routing、validation、queue、claim、event、retention 与 execution。断开 skill stream 不取消其 execution。

`WorkflowService` 提供 ActionList apply/get/list、atomic ActionList/fact cursor snapshot，以及 manager-wide ActionList/fact event replay。其 focused implementation boundary 记录在 [workflow adapter](northbound/workflow_cn.md)，wire contract 与 recovery algorithm 记录在 [WorkflowService](../../interfaces/workflow_cn.md)。

external north HTTP 进程为 browser route 与可选 non-authoritative audit projection 复用一个 generated Workflow stub。两个 consumer 都不链接进 EOS core，audit persistence 也绝不改变该 service 的 authoritative state。

## Workflow replay

`GetRuntimeSnapshot` 返回 independently sequenced skill execution，以及由 ActionList manager atomic copy 的 ActionList、fact 与 `action_list_cursor`。`WatchActionListEvents` replay required `{epoch, sequence}` cursor 之后的完整 ActionList revision 与 accepted fact，随后 follow live mutation。

core 默认保留 4,096 个 workflow event。epoch mismatch 或 same-epoch future sequence 映射为 `FAILED_PRECONDITION`；比 retained window 更旧的 cursor 映射为 `OUT_OF_RANGE`；missing 或 empty cursor 映射为 `INVALID_ARGUMENT`。这些 status 指示 client 获取 fresh snapshot 并 resume。Northbound 不 repair、skip、merge 或 synthesize domain event。

## Cross-cutting transport behavior

每个 inbound RPC 都在标准 request-correlation scope 中运行。`StartExecution` 与 ActionList target dispatch 把 correlation identity 保留到 asynchronous work。request cancellation 会结束属于该 call 或 stream 的 server work；它不会隐式取消 accepted skill execution、primitive command 或 ActionList。

server 为全部三个 service 启用标准 gRPC health 与 reflection。malformed transport input 映射为 `INVALID_ARGUMENT`，missing identity 映射为 `NOT_FOUND`，idempotency conflict 映射为 `ALREADY_EXISTS`，exhausted bounded storage 映射为 `RESOURCE_EXHAUSTED`，state/cursor epoch conflict 与 model-capability conflict 映射为 `FAILED_PRECONDITION`，expired workflow cursor 映射为 `OUT_OF_RANGE`，missing composed service 映射为 `UNAVAILABLE`，invalid model response 映射为 `DATA_LOSS`。primitive、skill 与 workflow semantic outcome 在 acceptance 后保持 typed successful payload。

## 日志与调试

`northbound` logger 记录 service、method、final numeric gRPC status、duration、correlation identity、已实现的 stream open/close，以及有界 transport failure reason。它绝不记录 argument、result、schema、fact、ActionList、event payload、cursor value、resource payload、primitive acknowledgement、credential 或 raw protobuf。

正常 unchanged resource sample、empty event-follow wait 与 polling loop 保持静默。把 `northbound` request completion 与 owning `dispatch`、`skills` 或 `actionlist` domain logger 关联。对于 workflow recovery，client 必须保留返回的 gRPC status 与其 last applied cursor，因为 cursor value 有意不复制到 server log。

## Lifecycle 与 configuration

`NorthboundServer::Run()` 绑定 listener、启动 tick thread、报告 ready，并阻塞在 gRPC serving。`Stop()` 在 referenced domain service 被销毁前关闭 gRPC 并 join tick thread。

```sh
./bin/eosd \
  --contracts /path/to/share \
  --skills-dir /path/to/native-skills \
  --host 127.0.0.1 \
  --grpc-port 16200
```

address 来自 `[gateway] host` 与 `grpc_port`；command-line flag 覆盖它。default 是 loopback。历史 listener 不提供 authentication 或 TLS；已合并的 EOS R03 实现在受管理写入上增加本机 Bearer 授权校验，但不增加 TLS 或网络安全边界，因此 deployment 不得在缺少 external security boundary 时把它绑定到 untrusted interface。

## 测试

Generated-stub integration test 覆盖全部三个 service、标准 health/reflection、resource stream cancellation、skill replay/cancellation、Workflow lifecycle 与 snapshot conversion、live `WatchActionListEvents`，以及 workflow epoch rejection。ActionList unit test 在 adapter 下方覆盖 cursor expiry 与 snapshot atomicity。

Contract-interface test 验证 protobuf source 生成的 service 与 field descriptor。lifecycle test 在 EOS startup 时占用 legacy HTTP port，以确认 core 只打开 configured gRPC listener。

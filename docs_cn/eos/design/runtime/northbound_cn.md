# Northbound 运行时

[运行时总览](README_cn.md) · [接口总览](../../develop/interfaces/README_cn.md) · [Workflow adapter](northbound/workflow_cn.md) · [English](../../../../docs/eos/design/runtime/northbound.md)

`src/eos/runtime/northbound/` 在一个原生 gRPC listener 上提供 `island.eos.v1.GatewayService`、`island.eos.v1.SkillService` 与 `island.eos.v1.WorkflowService`。EOS core 不嵌入 HTTP listener。contract 位于 `share/contracts/eos/gateway.proto`、`share/contracts/eos/skills.proto` 与 `share/contracts/eos/workflow.proto`。

Northbound 是 transport adapter。Perception、Dispatcher、SkillManager 与 ActionList 保持 authoritative domain owner；protobuf conversion 与 gRPC lifecycle 不会把其 state 移入 transport。

## Module map

| Source folder | Documentation | Responsibility |
| --- | --- | --- |
| `runtime/northbound/` | 本文 | shared server、Gateway adapter、Skill adapter、codec、resource subscription、health 与 reflection |
| `runtime/northbound/workflow/` | [workflow_cn.md](northbound/workflow_cn.md) | `WorkflowService` unary conversion 与 ActionList event streaming |

workflow child module 有自己的 `CMakeLists.txt`，并把其 service implementation 加入 `eos_northbound`。shared request context、duration validation、listener lifecycle 与 generated contract 保留在 northbound boundary。

## Services

`GatewayService` 提供 `Discover`、`GetStatus`、`ListResources`、`ReadResource`、`WatchResource`、`CallPrimitive`、`CancelPrimitive`、`CaptureFrames`，以及 typed unary `InferVad`、`DetectHumans`、`EstimateHumanPose` 与 `DetectFaces`。每次 inference 都先解析 request 指定的精确 model ID 与 capability，再调用 models runtime，并把 protobuf bytes 转为 canonical model contract 的 base64 field，同时协作式传递 gRPC cancellation。Face registration 不属于该 inference surface，仍是 primitive。`NorthboundCore` 只拥有 transport cache：bounded two-frame camera cache，以及 `WatchResource` 使用的 sampled-content tracker。Perception 与 Dispatcher 拥有 underlying data 与 action semantic。

`SkillService` 提供 catalog discovery、process-like execution management、SIGNAL admission、lifecycle replay，以及 unified lifecycle/outbound-SIGNAL event watch。SkillManager 拥有 exact runtime routing、validation、queue、claim、event、retention 与 execution。断开 skill stream 不取消其 execution。

`WorkflowService` 提供 ActionList apply/get/list、atomic ActionList/fact cursor snapshot，以及 manager-wide ActionList/fact event replay。其 focused implementation boundary 记录在 [workflow adapter](northbound/workflow_cn.md)，wire contract 与 recovery algorithm 记录在 [WorkflowService](../../develop/interfaces/workflow_cn.md)。

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
  --grpc-port 16003
```

address 来自 `[gateway] host` 与 `grpc_port`；command-line flag 覆盖它。default 是 loopback。listener 不提供 authentication 或 TLS，因此 deployment 不得在缺少 external security boundary 时把它绑定到 untrusted interface。

## 测试

Generated-stub integration test 覆盖全部三个 service、标准 health/reflection、resource stream cancellation、skill replay/cancellation、Workflow lifecycle 与 snapshot conversion、live `WatchActionListEvents`，以及 workflow epoch rejection。ActionList unit test 在 adapter 下方覆盖 cursor expiry 与 snapshot atomicity。

Contract-interface test 验证 protobuf source 生成的 service 与 field descriptor。lifecycle test 在 EOS startup 时占用 legacy HTTP port，以确认 core 只打开 configured gRPC listener。

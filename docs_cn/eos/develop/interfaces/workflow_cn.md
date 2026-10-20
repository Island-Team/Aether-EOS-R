# WorkflowService gRPC 接口

[Northbound 接口](README_cn.md) · [ActionList 运行时](../../design/runtime/actionlist_cn.md) · [SkillService](../../../../docs/eos/develop/interfaces/skills.md) · [Primitive call](../../../../docs/eos/develop/interfaces/actions.md) · [English](../../../../docs/eos/develop/interfaces/workflow.md)

`island.eos.v1.WorkflowService` 是用于提交、检查、snapshot 和 follow bounded action list 的原生 gRPC API。Agent 选择完整 plan。EOS 验证 dependency graph，并机械式启动每个 eligible action；它不是 planner，也不会发明 next action。

该 service 与 `GatewayService` 和 `SkillService` 共用 `eosd` gRPC listener。本文仅定义 gRPC contract。

## RPC 清单

| RPC | Request | Response | Purpose |
| --- | --- | --- | --- |
| `ListWorkflowTemplates` | `ListWorkflowTemplatesRequest` | `ListWorkflowTemplatesResponse` | 发现 deployment 注册的 workflow template |
| `GetWorkflowTemplate` | `GetWorkflowTemplateRequest` | `WorkflowTemplateDescriptor` | 按 name 读取一个 template |
| `RunWorkflowTemplate` | `RunWorkflowTemplateRequest` | `ActionList` | 复制、验证并启动注册 template |
| `ApplyActionList` | `ApplyActionListRequest` | `ActionList` | 验证、接受并开始 plan |
| `GetActionList` | `GetActionListRequest` | `ActionList` | 按 `workflow_id` 读取一个 plan |
| `ListActionLists` | `ListActionListsRequest` | `ListActionListsResponse` | 读取 retained plan |
| `GetRuntimeSnapshot` | `GetRuntimeSnapshotRequest` | `ManagerRuntimeSnapshot` | bootstrap execution、fact、plan 与 event cursor |
| `WatchActionListEvents` | `WatchActionListEventsRequest` | stream `ActionListEvent` | replay 并 follow ActionList 与 accepted-fact mutation |

空 unary request 使用默认 protobuf instance。可发送可选 `x-request-id` metadata 用于 log correlation，其值为 1–96 个 ASCII letter、digit、`.`、`_`、`:` 或 `-`。call deadline 与 cancellation 使用普通 gRPC semantic。断开 `WatchActionListEvents` 只停止该 stream，不取消 action。

## 注册 template

EOS 在 startup barrier 解析 deployment `share/workflows` 下的每个 `*.json` 文件。 文件内容是 `WorkflowDefinition`，文件名 stem 是 catalog name；malformed template 会 阻止 EOS ready。discovery RPC 不创建 ActionList。`RunWorkflowTemplate` 复制 immutable definition 并通过与 `ApplyActionList` 相同的 manager path 提交；可选 `workflow_id` 覆盖 authoring ID，未提供时 EOS 追加 fresh UUID。

## 应用 definition

`ApplyActionListRequest.definition` 是必需的：

| Field | Type | Rules |
| --- | --- | --- |
| `workflow_id` | string | 必需且非空 |
| `actions` | repeated `WorkflowAction` | 必需、非空，并具有唯一的非空 `action_id` |
| `supersedes_workflow_ids` | repeated string | 要退役的已有 eligible list；self-reference 与 duplicate 无效 |

每个 `WorkflowAction` 包含：

| Field | Type | Rules |
| --- | --- | --- |
| `action_id` | string | 在 definition 内唯一 |
| `skill` | `SkillReference` | 恰好一个 target；exact `name` 与 `version` |
| `primitive` | `PrimitiveActionTarget` | 恰好一个 target；exact `name`、`version` 与 `device_id` |
| `control` | `ActionControlTarget` | 恰好一个 target；终止同一 list 中的另一个 action |
| `noop` | `NoopActionTarget` | 恰好一个 target；无 external side effect 地成功 |
| `parameters` | `google.protobuf.Struct` | 由 selected owner 验证的 skill 或 primitive argument |
| `depends_on` | repeated string | 每个 ID 必须存在，可以出现在所属 action 之前或之后，且完整 graph 必须 acyclic |
| `when` | `StateCondition` | 可选 latest-fact 或 detection-resource equality guard |
| `output_bindings` | repeated `ActionOutputBinding` | 从 earlier successful output 复制到 parameter template |
| `choice_group` | string | 可选 mutually exclusive branch group；必须有 `when` |
| `execution_mode` | `ActionExecutionMode` | 可选 `TASK` 或 `MONITOR`；unspecified 等同 task |
| `monitor_lifetime` | `MonitorLifetime` | monitor 必需：`EXPLICIT` 或 `WORKFLOW` |

fact condition 必须提供 `fact_key`；可选 `source_action_id` 将 fact 限制为该 action 创建的 execution。resource condition 改为必须提供 `resource_uri`、`detection_kind` （`human` 或 `object`）与 allowlisted `detection_value_pointer`，并在 latest resource snapshot 的 detection 上求值。两种 condition 互斥。`equals` 使用 protobuf `Value` equality。可选 non-negative `max_age` 使用 fact event 或 resource observation time。 age 会在后续 scheduling event 时重新评估；仅 expiration 不创建 timer 或 revision。

```text
definition {
  workflow_id: "greet-visitor-42"
  actions {
    action_id: "observe"
    skill { name: "observe_human" version: "1.0.0" }
    parameters { fields { key: "robot_name" value { string_value: "robot-0" } } }
  }
  actions {
    action_id: "greet"
    skill { name: "speak" version: "1.0.0" }
    depends_on: "observe"
    when {
      fact_key: "human.present"
      equals { bool_value: true }
      max_age { seconds: 5 }
    }
  }
}
```

output binding 的两个 pointer 使用 RFC 6901；source 必须在 array 中早于 destination， 并隐式增加 dependency。destination 必须指向 submitted object parameter 中已经存在且 不与其他 binding overlap 的位置。同一非空 `choice_group` 的 action mutually exclusive； 第一个 eligible member 被选择，其他仍 blocked/ready 的 member 变成 `SKIPPED`。noop 用于没有 external effect 的 branch；control 当前仅支持 `TERMINATE`。

`MONITOR` 一旦 queued/running 就满足普通 dependency；output binding 仍要求真正 `SUCCEEDED`。`EXPLICIT` monitor 由 control action 终止；`WORKFLOW` monitor 在所有 非 workflow-monitor action 完成后自动 cooperative stop，并在 workflow failure 时停止。 请求过 stop 的 monitor cancellation 映射成成功的 `STOPPED`。control 与 noop 不能是 monitor。

structural validation 是 atomic 的：malformed action、duplicate ID、unknown 或 self dependency/control target/condition source、cycle、invalid binding/pointer、condition、 choice 或 monitor declaration 都会在存储前拒绝 definition。target identity 与 resolved parameter validation 在 eligible action 进入 owning SkillManager 或 Dispatcher boundary 时发生；target rejection 保留为 terminal `REJECTED` result。

## 调度与状态

当所有普通 dependency 为 `SUCCEEDED`、monitor dependency 已 active 或成功 stopped，且 可选 condition 匹配 sufficiently fresh fact/resource snapshot 时，action 才 eligible。 所有 eligible action 可以并发启动，但仍受 SkillManager claim 与 Dispatcher admission 约束。skill target 创建 `SkillExecution`；primitive target 获得 EOS 生成的 `command_id`。除 binding source 必须早于 destination 外，array position 没有含义。

`ActionItem` 报告 submitted `action`、`status`、可选 `execution_id` 或 `command_id`、 terminal legacy `result`、normalized JSON `output`、immutable `resolved_parameters` 与 bounded `reason`。Action 状态另含 `SKIPPED` 与 `STOPPED`：前者表示另一个 choice member 被选中，后者表示 monitor 成功到达声明的 stop point。

`ActionList` 包含 `workflow_id`、monotonically increasing `revision`、derived `status`、完整 item array、creation/update timestamp、supersession input 与可选 `superseded_by_workflow_id`。List 状态为 `PENDING`、`RUNNING`、`SUCCEEDED`、`FAILED`、`SUPERSEDING` 和 `SUPERSEDED`。client 使用较新的完整 revision 替换 cached ActionList，而不合并 item array。

任何 unsuccessful terminal action 都会使 list 变为 `FAILED`，并阻止后续 blocked 或 ready sibling dispatch。已运行 sibling 不会自动取消。supersession 请求 cooperative cancellation、阻止旧 list 的新 work、保留两个 list，并最终把旧 list 转为 `SUPERSEDED`。EOS 永远不会重试 rejected、failed、cancelled、timed-out 或 outcome-unknown action。

## Event 与 cursor

`ActionListCursor` 包含 opaque boot `epoch` 与 unsigned `sequence`。每次构造 ActionList manager 时，EOS 都生成新 epoch。空 snapshot 中 sequence 从零开始，并对该 epoch 中每个 retained `ActionListEvent` 增加一。cursor 标识 client 已经纳入的最后 mutation；replay 是 exclusive 的，只返回更大 sequence 的 event。

`ActionListEvent` 包含 `cursor`、server `observed_at`，以及恰好一个 payload：

- `action_list`：admitted、replaced、superseded、dispatched、progressed 或 terminal mutation 后的完整 `ActionList` revision；按其 `workflow_id` 替换 cached object。
- `fact`：该 key 对应的完整 accepted `SystemStateFact`；替换该 key 的 cached latest fact。

被拒绝的 stale 或 duplicate fact 不生成 event。Event payload 是完整 state projection，而不是 patch。stream 在一个 total ActionList-domain sequence 中交错两种 payload kind。

core 默认跨两种 payload kind 保留最新 4,096 个 event。retention 位于内存并按 count 限制，而不是按时间。即使 EOS process 没有重启，高 mutation volume 也可能使 disconnected client 的 cursor 过期。

`WatchActionListEventsRequest.after` 是必需的，且其 `epoch` 必须非空。server 先 replay retained event 中 sequence 大于 `after.sequence` 的部分，再等待 live event。只使用 EOS 发出的 cursor；大于 latest published sequence 的 same-epoch sequence（包括 `uint64` maximum）会被拒绝为 `FAILED_PRECONDITION`，而不是等待或 overflow。

## Runtime snapshot 与 atomicity

`ManagerRuntimeSnapshot` 包含 `captured_at`、retained `SkillExecution`、latest `SystemStateFact`、retained `ActionList` 与 `action_list_cursor`。fact、ActionList 和 `action_list_cursor` 在同一个 ActionList state lock 下复制。因此，该 snapshot 纳入的每个 ActionList/fact mutation 的 sequence 都小于等于 cursor，而后续每个 mutation 都可通过 event path 获得，除非 retention 过期。

skill execution 在 ActionList state copy 前从 SkillManager 获取，因此 aggregate snapshot 不是跨 SkillManager 与 ActionList 的 atomic transaction。使用 execution sequence 判断 execution freshness，并且只把 `action_list_cursor` 用于 ActionList/fact recovery。

`SystemStateFact` 公开 `key`、JSON-compatible `value`、`source`、`source_execution_id`、`source_sequence`、`event_time` 与 `received_time`。当前 runtime 把 `source` mirror 到 `source_execution_id`；`frame_id`、`unit`、`confidence` 与 `validity` 保持 protobuf default，不得用于 decision。

## Resnapshot 与 resume algorithm

initial connection 与每次 recovery 都使用以下顺序：

1. 调用 `GetRuntimeSnapshot`，使用完整 response 替换本地 retained ActionList 与 latest-fact view。
2. 只有在 snapshot 已应用到本地后，才存储 `action_list_cursor`。
3. 在 `after` 中使用该 cursor 打开 `WatchActionListEvents`；atomic ActionList snapshot copy 后提交的 mutation 会无 gap replay。
4. 按 stream 顺序应用每个完整 payload，随后把 stored cursor 推进到该 event 的 `cursor`。client 只有在 payload 已 durably applied 后才 persist 或 acknowledge cursor。
5. ordinary disconnect 或 deadline 后，使用最后已应用 cursor 重连。exclusive replay 避免重新发送该 event。
6. 遇到 `FAILED_PRECONDITION` 或 `OUT_OF_RANGE` 时，丢弃 incremental assumption，获取新 `GetRuntimeSnapshot`、替换本地 state，并从其新 cursor resume。

client 应验证一个 stream 内 epoch 一致且 sequence strict increasing。repeated event 可以安全替换相同完整 cached object，但 client 不得仅根据 ActionList revision 推断 missing event，因为 fact event 共享该 sequence。

## Error 与 client 行为

主要 status 为：

- `INVALID_ARGUMENT`：malformed definition/request、缺失 `workflow_id`、缺失 `after` 或空 cursor epoch。
- `NOT_FOUND`：请求的 `workflow_id` 不存在。
- `FAILED_PRECONDITION`：无效 supersession/replacement state、cursor epoch 与本次 EOS boot 不同，或 same-epoch cursor sequence 位于 future。
- `OUT_OF_RANGE`：cursor sequence 比 retained event window 更旧。
- `UNAVAILABLE`：必需 composed runtime dependency 不可用。
- `INTERNAL`：conversion failure、broken invariant 或 unexpected exception。

accepted target failure 保持成功的 typed `ActionList`/`ActionListEvent` payload，而不是 transport failure。restart 后 missing workflow 表示 process-local state 丢失；它不能证明 physical effect 已反转。只有在不关心其他 retained workflow 或 fact 时，才因为一个 terminal list 停止 follow；`WatchActionListEvents` 是 manager-wide stream，不会在单个 workflow terminal 时关闭。

## 日志与调试

每个 RPC 都在标准 correlation scope 中运行。`northbound` logger 以 `grpc_request_completed` 或 `grpc_request_rejected` 记录 `service: "WorkflowService"`、method、final gRPC status 与 total duration；不记录 definition、ActionList、fact、event payload 或 raw protobuf。long-lived watch 在 stream 关闭时发出 request completion record。

domain mutation 继续通过 payload-free `actionlist` logger 可见。对于 recovery failure，在 client 中记录 method、返回的 gRPC status、cursor epoch 与 sequence；EOS log 特意避免为每个 streamed event 重复 cursor 或 payload data。正常空 250 ms follow wait 保持静默。

## 测试

`eos_actionlist_tests` 除 DAG scheduling behavior 外，还覆盖 initial 与 monotonic cursor、完整 ActionList 与 fact event、snapshot/cursor consistency、exclusive replay、live fact follow、bounded-retention expiry、epoch mismatch 与 future-cursor rejection。

Northbound integration test 覆盖 protobuf conversion、snapshot cursor 后的 live `WatchActionListEvents` delivery、stream cancellation、`FAILED_PRECONDITION` epoch mismatch、`GetRuntimeSnapshot.action_list_cursor`、health 与 reflection。contract-interface test 验证 server-streaming RPC 保持由 `workflow.proto` 生成；core unit test 对 expired 与 future cursor 保持权威。

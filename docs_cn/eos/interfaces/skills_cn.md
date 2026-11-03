# SkillService 进程接口

[English](../../../docs/eos/interfaces/skills.md) · [接口总览](README_cn.md) · [Workflow](workflow_cn.md) · [Primitive](actions_cn.md)

`island.eos.v1.SkillService` 在 `share/eos/contracts/eos/skills.proto` 中定义。它向 Agent 提供技能发现和异步执行，与原生技能编写 ABI 分离；没有同步 run-to-completion RPC。`ListSkills` 与 `GetSkill` 读取 catalog，`StartExecution` 创建执行，`GetExecution`、`ListExecutions`、`WatchExecution` 和 `FollowExecutionJournal` 读取状态，`SignalExecution` 发送应用 SIGNAL，`TerminateExecution` 与 `TerminateExecutions` 请求协作终止，`MonitorExecutions` 提供全局筛选视图。终态记录在有界 retention 内仍可查询。

## R04 默认 Rust 子集——实现切片

以上是已发布 protobuf 身份；本页后续旧语义描述可选 C++ SkillManager，不等于默认 Rust 支持。[R04 宿主契约](skills/native/host-abi_cn.md)规定选择原生库的边界。已合并的默认 Rust runtime 在目录准入后服务下列七方法；未配置外部宿主时，可执行目录发现不可用。R04 不改变 proto package、service、method、字段编号或 ProtoJSON 名称。

| 已发布 RPC | R04 Rust 行为 |
| --- | --- |
| `ListSkills` | 已准入、排序的目录；未配置宿主时为空。 |
| `GetSkill` | 精确 `{name,version}`，否则 `NOT_FOUND`。 |
| `StartExecution` | 带授权和修订检查的幂等创建，返回首个 `SkillExecution` 快照。 |
| `GetExecution` | 按不透明 `execution_id` 读取授权快照。 |
| `WatchExecution` | 从 `after_sequence` 重放授权生命周期，再跟随至终态。 |
| `ListExecutions` | 有界的当前授权保留快照。 |
| `TerminateExecution` | 带授权和修订检查的取消请求及当前快照。 |
| `GetExecutionState` | `UNIMPLEMENTED`。 |
| `WatchExecutionState` | `UNIMPLEMENTED`。 |
| `TerminateExecutions` | `UNIMPLEMENTED`。 |
| `SignalExecution` | `UNIMPLEMENTED`。 |
| `FollowExecutionJournal` | `UNIMPLEMENTED`。 |
| `MonitorExecutions` | `UNIMPLEMENTED`。 |

只有已选择清单、所有库、完整目录及七个 handler 都准入后，R04 才将 `island.eos.v1.SkillService` 的 gRPC health 标为 `SERVING`。空选择是有效空目录；无效非空选择在监听器就绪前失败。health 不会使未实现的方法可调用；其结果仍是 `UNIMPLEMENTED`。`WorkflowService` 保持不可用，聚合 health 仍要求 R02/R03 的发现和 Dispatcher 先决条件。完整 ListSkills 与 ListExecutions 响应总量限制为 3 MiB，以适配 EGIA 当前默认 4 MiB 接收上限；后者只淘汰终态记录。Watch 重放丢失时返回 `OUT_OF_RANGE`，不会静默跳过生命周期转换。

R04 沿用 R03 gRPC metadata。`ListSkills`/`GetSkill` 不需要 grant。`StartExecution`/`TerminateExecution` 必须带 `authorization: Bearer <64 位小写十六进制 EGIA grant>` 与 `x-eos-active-revision: <64 位小写十六进制保存图修订>`，不新增 proto 字段或公开 grant。缺失/畸形/未知 grant 返回 `UNAUTHENTICATED`；有效 `operator-control` 返回 `PERMISSION_DENIED`；缺失/畸形修订返回 `INVALID_ARGUMENT`；格式正确但过时返回 `FAILED_PRECONDITION`，均在 worker 启动前。`GetExecution`/`WatchExecution`/`ListExecutions` 要求当前运行的 `egia-control` grant，但无需当前修订。缺失 grant 是 `UNAUTHENTICATED`；有效错误角色或不可访问、旧运行、不存在的单项 execution 都是统一不透明 `NOT_FOUND`。List 只返回该角色及该运行的记录。watch 断开不取消执行。从 Discover 的 `eos-active-revision` 元数据取得保存修订后，可沿用现有 ProtoJSON 请求 `{"reference":{"name":"example.observe","version":"1.0.0"},"arguments":{},"requestId":"egia-step-17","timeout":"30s"}` 并加上述两项 metadata。响应 `executionId` 是 EOS 执行身份，与 IsOS 部署 `run_id` 及幂等 `requestId` 均不同。

执行器可能运动但没有终态结果时，公开状态是 `EXECUTION_STATUS_FAILED`，稳定 `reason_code = "worker.outcome_unknown"`；proto 没有 UNKNOWN execution status。EOS 保持 Dispatcher movement inhibit，不自动重试。R04 的已声明 primitive callback 必须经 R03 Dispatcher。没有完整 Rust 所有者的 resource/source/inference/SIGNAL/community 或整次执行 claim 声明在目录准入时整体拒绝，不能发布虚假可执行技能；未声明家族的可选 ABI callback 可保持缺席。已准入但 `reentrant=false` 的技能按精确身份串行；排队中的 Start 可以在启动 worker 前过期或取消。WorkflowService、ActionList、图技能序列及子进程 callback 不在 R04 内。参见 [R04 设计](../design/runtime/skills/host-rust_cn.md)、[首次使用](../develop/host-skills_cn.md)和[用例](../../../tests/fixtures/r04_host_skills/README_cn.md)。

## Catalog 与启动

`ListSkills` 返回按精确 name/version 排序的已接纳 descriptor；`GetSkill` 查找一个精确身份。Descriptor 包含参数和结果 JSON Schema、primitive 与资源声明、接收/发出的 SIGNAL schema、claim 模板、允许的推理权限和 reentrancy。它不暴露原生库路径或提供注册接口。空 catalog 是有效状态。

`StartExecution` 要求精确的技能身份、符合参数 schema 的 object-valued `arguments`，以及 1–96 字节的非空 `request_id`。参数 JSON 最多 1 MiB。可选 `timeout` 为正 `Duration`，上限一小时；省略时无 deadline。请求 ID 在 manager 中全局去重：内容相同返回原执行，内容冲突返回 `ALREADY_EXISTS`。去重仅持续到进程重启或记录被淘汰。响应是首个完整 `SkillExecution` snapshot。

```protobuf
StartExecutionRequest {
  reference { name: "example.observe" version: "1.0.0" }
  arguments { fields { key: "robot_name" value { string_value: "robot-0" } } }
  request_id: "egia-plan-8-step-2"
  timeout { seconds: 30 }
}
```

## 快照、watch 与 SIGNAL

`GetExecution` 返回当前完整快照。`WatchExecution(after_sequence)` 先重放更高 sequence 的 retained 生命周期快照，再跟随新状态并在终态后关闭。零从最早保留记录开始；断线只结束 stream，不取消执行。状态从 `QUEUED` 到 `RUNNING`，最后进入 `SUCCEEDED`、`REJECTED`、`CANCELLED`、`TIMED_OUT` 或 `FAILED`。快照还提供 execution/request ID、claim、队列原因、取消标记、时间、结果、诊断和计数。客户端应依据状态及 reason code，而非解析人类诊断文本。

`SignalExecution` 需要执行 ID、独立的幂等 request ID、descriptor 中声明的 SIGNAL 名称和符合 schema 的对象 payload。Receipt 只确认验证并入队，不代表技能已经消费。`CANCEL` 是 manager 保留名，必须改用 `TerminateExecution`。同一执行中的入站 SIGNAL 有序且最多保留 256 条；EOS 不提供 topic、broadcast 或跨技能直接转发，外部 Agent 负责路由。

```protobuf
SignalExecutionRequest {
  execution_id: "0199..."
  request_id: "egia-plan-8-signal-3"
  name: "dialogue.response"
  payload { fields { key: "text" value { string_value: "Hello." } } }
}
```

`FollowExecutionJournal(after_journal_sequence)` 混合生命周期快照、发出的 SIGNAL 与 journal record；它使用独立于 `WatchExecution` 的 cursor。处理事件成功后再保存 cursor。统一历史默认保留最多 1,024 条；过旧 cursor 返回当前可用后缀。`MonitorExecutions` 在连接时发送 `reset = true` 的匹配快照，随后以全局递增 sequence 发送 upsert/remove；历史默认保留 4,096 条更新。`TerminateExecutions` 按精确技能选择当前 queued/running 的执行，并以 request ID 固定目标集合。

## 错误与边界

无效身份、参数、SIGNAL 或 cursor 返回 `INVALID_ARGUMENT`；找不到技能或执行返回 `NOT_FOUND`；幂等 ID 内容冲突返回 `ALREADY_EXISTS`；向终态执行发送 SIGNAL 返回 `FAILED_PRECONDITION`；容量满返回 `RESOURCE_EXHAUSTED`。技能自身拒绝和执行失败属于正常终态快照，而不是 transport error。记录、幂等键和执行都只在当前进程及有界 retention 内有效。完整状态字段、claim 冲突、错误及重连语义见 [英文接口](../../../docs/eos/interfaces/skills.md)。

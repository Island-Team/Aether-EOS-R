# SkillService 进程接口

[English](../../../../docs/eos/develop/interfaces/skills.md) · [接口总览](README_cn.md) · [Workflow](workflow_cn.md) · [Primitive](actions_cn.md)

`island.eos.v1.SkillService` 在 `share/contracts/eos/skills.proto` 中定义。它向 Agent 提供技能发现和异步执行，与原生技能编写 ABI 分离；没有同步 run-to-completion RPC。`ListSkills` 与 `GetSkill` 读取 catalog，`StartExecution` 创建执行，`GetExecution`、`ListExecutions`、`WatchExecution` 和 `FollowExecutionJournal` 读取状态，`SignalExecution` 发送应用 SIGNAL，`TerminateExecution` 与 `TerminateExecutions` 请求协作终止，`MonitorExecutions` 提供全局筛选视图。终态记录在有界 retention 内仍可查询。

## Catalog 与启动

`ListSkills` 返回按精确 name/version 排序的已接纳 descriptor；`GetSkill` 查找一个精确身份。Descriptor 包含参数和结果 JSON Schema、primitive 与资源声明、接收/发出的 SIGNAL schema、claim 模板、允许的推理权限和 reentrancy。它不暴露原生库路径或提供注册接口。空 catalog 是有效状态。

`StartExecution` 要求精确的技能身份、符合参数 schema 的 object-valued `arguments`，以及 1–96 字节的非空 `request_id`。参数 JSON 最多 1 MiB。可选 `timeout` 为正 `Duration`，上限一小时；省略时无 deadline。请求 ID 在 manager 中全局去重：内容相同返回原执行，内容冲突返回 `ALREADY_EXISTS`。去重仅持续到进程重启或记录被淘汰。响应是首个完整 `SkillExecution` snapshot。

```protobuf
StartExecutionRequest {
  reference { name: "observe_human" version: "1.0.0" }
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

无效身份、参数、SIGNAL 或 cursor 返回 `INVALID_ARGUMENT`；找不到技能或执行返回 `NOT_FOUND`；幂等 ID 内容冲突返回 `ALREADY_EXISTS`；向终态执行发送 SIGNAL 返回 `FAILED_PRECONDITION`；容量满返回 `RESOURCE_EXHAUSTED`。技能自身拒绝和执行失败属于正常终态快照，而不是 transport error。记录、幂等键和执行都只在当前进程及有界 retention 内有效。完整状态字段、claim 冲突、错误及重连语义见 [英文接口](../../../../docs/eos/develop/interfaces/skills.md)。

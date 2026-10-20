# Skill process model

[总览](../skills_cn.md) · [IPC](../../../develop/interfaces/skills/native/ipc_cn.md) · [Authoring](authoring_cn.md) · [English](../../../../../docs/eos/design/runtime/skills/process-model.md)

SkillManager 把每次 invocation 当作 supervised logical process。definition 是类似 executable 的 catalog entry，opaque execution ID 是类似 PID 的 identity。这一类比提供 ownership 与 inspection，不提供 OS isolation。

## Process table

manager 在 worker admission 前创建 execution record，并持有至 terminal retention 结束。snapshot 标识 exact skill/runtime、request、parent execution、boot、lifecycle、deadline、timestamp、claim、activity、operation、accounting 与 terminal cause。现有 `Get/List/WatchExecution` API 是 public process-table view。

Built-in definition 作为 trusted C++20 coroutine 在 `eosd` 内运行。等待 capability、mailbox、child、source、timer 或 claim 会 suspend coroutine，不占用 runnable-skill worker。wrapper 之外的 blocking code 无法安全 preempt，属于 author error。

## Scheduling 与 operation

默认有 4 个 runnable-skill worker、8 个 capability-operation worker；每个 execution 最多 64 个、全局最多 256 个 outstanding primitive operation。每个 operation 有独立 ID、lifecycle、timing、claim、cancellation 与 result。不同 device 可并行；同一 device 保留 Dispatcher FIFO 与 safety admission。

Non-reentrancy 按 exact skill identity 与 runtime scoped；显式 device/resource claim 保持 global。queue deadline 不依赖 worker wakeup，observer callback 使用独立 bounded capacity。

## Parent/child rule

`spawn` 创建带 `parent_execution_id` 的普通 execution。tree 最大深度 8，每个 parent 最多 32 个 live child。self-spawn、ancestor cycle、unrelated handle、detached child 与会 deadlock 的 claim combination 都被拒绝。parent completion、cancellation、timeout 与 shutdown 会请求取消 active descendant。child failure 由 `await_exit` 返回，不自动决定 parent outcome。

Built-in termination 是 cooperative 的。address-space isolation、cgroup、crash containment 与 hard kill 只能由未来 external runtime 提供。

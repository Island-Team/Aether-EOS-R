# Skill IPC

[总览](../../../../design/runtime/skills_cn.md) · [Process model](../../../../design/runtime/skills/process-model_cn.md) · [Data 与 time](../../../../design/runtime/skills/data_cn.md) · [English](../../../../../../docs/eos/develop/interfaces/skills/native/ipc.md)

Built-in skill 通过 manager-owned mailbox 通信，不共享 mutable object，也不获取任意 execution handle。definition 声明每个 channel、JSON Schema 与 send/receive authority；catalog admission 拒绝无效或重复 declaration。

author API 提供 `spawn`、`send`、`receive`、`request`、`await_exit` 与 `terminate`。handle 仅对 parent/child relationship 有效。`request` 在相同 mailbox machinery 上使用 request/correlation ID，并非隐藏 direct call。Public Agent SIGNAL 保持兼容，并逐步复用相同 delivery machinery。

## Envelope 与 delivery

`IpcMessage` 携带 message/request/correlation/causation ID、sender/recipient execution ID、channel/schema version、JSON object payload、canonical `EventTime` 与 optional deadline。diagnostic 仅记录 safe payload size 与 digest，默认不记录 raw body。

mailbox 默认最多 256 条 pending message。send 在 mailbox 满时最多等待其 TTL，过期条目会被清理；receive 选择匹配 channel 的第一条保留消息。未声明的方向、无效 schema、mailbox 满、inactive endpoint、重复 request 冲突或无效关系都会明确失败。不保证 global order 或 implicit broadcast。

mailbox 只存在于进程内，restart 不恢复。当前实现不承诺 IPC journal record 或跨进程 transport delivery。共享内存传输与外部 skill-host 协议仍属于未来工作。

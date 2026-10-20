# Skill IPC

[Overview](../../../../design/runtime/skills.md) · [Process model](../../../../design/runtime/skills/process-model.md) · [Data and time](../../../../design/runtime/skills/data.md) · [中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/ipc_cn.md)

Built-in skills can communicate through process-local, manager-owned parent/child mailboxes. They do not share mutable objects or obtain arbitrary execution handles. Definitions declare every channel, its JSON Schema, and send/receive authority; catalog admission rejects invalid or duplicate declarations.

The author API provides `spawn`, `send`, `receive`, `request`, `await_exit`, and `terminate`. Handles are valid only for the parent/child relationship. `request` is implemented over the same mailbox operations; it is not a hidden direct call. Public Agent SIGNAL behavior remains separate.

## Envelope and delivery

An `IpcMessage` carries message, request, correlation, and causation IDs; sender and recipient execution IDs; channel and schema version; JSON object payload; `EventTime`; and optional deadline.

Mailboxes contain at most 256 pending messages by default. Send waits up to its TTL when the mailbox is full, expired entries are pruned, and receive selects the first retained message matching the requested channel. Undeclared direction, invalid schema, full mailboxes, inactive endpoints, duplicate request conflicts, and invalid relationships fail closed.

Mailboxes are memory-only and are not restored after restart. The current implementation does not promise IPC journal records, transport delivery, global ordering, or implicit broadcast.

This mailbox API is process-local. Shared-memory transport and an out-of-process skill-host protocol are future work; neither is implied by the current `IpcMessage` envelope.

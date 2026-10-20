# External skill runtime

[Skill runtime overview](../skills.md) · [SkillManager supervisor](manager.md) · [Skill authoring](../../../develop/interfaces/skills/native/README.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/external_cn.md)

The external runtime folder reserves ownership for supervised out-of-process skills. It is a **future** isolation boundary, not the mailbox IPC used between built-in logical processes. It is fail-closed and contributes no `SkillRuntime` today.

## Ownership boundary

`src/eos/runtime/skills/external/` is the only location for a future process supervisor, sandbox policy, capability-scoped IPC protocol, termination control, and external SDK bridge. Process creation, IPC framing, capability enforcement, and lifecycle supervision must remain outside profile-neutral SkillManager and native loading code.

## Current behavior

The folder contributes an empty CMake interface target only. `eosd` launches no skill subprocess, opens no external-skill IPC endpoint, loads no Python runtime, and discovers no external package. No external package can be admitted, registered, or executed.

```mermaid
flowchart LR
  package["external package"] -->|"not admitted"| placeholder["external placeholder"]
  placeholder -.->|"no SkillRuntime"| manager["SkillManager"]
```

## Admission and execution

There is no external admission or execution API. A skill identity absent from registered runtimes fails exact lookup; SkillManager never spawns a process or falls back from another runtime. External-looking files, inherited descriptors, sockets, and environment variables are ignored by this placeholder.

## Logging and debugging

The placeholder starts no supervisor and emits no external lifecycle records. An attempted start for an unregistered external identity is reported through the normal `skills`/northbound unknown-skill path without logging package or environment contents. The absence of an external `SkillRuntime` in startup composition is the authoritative debug fact.

## Testing

Build and composition tests verify that the interface target links no launcher and that external-looking inputs do not appear in the aggregate catalog. SkillManager unknown-identity tests verify fail-closed routing. Process, IPC, sandbox, and forced-termination tests are out of scope until an implementation exists.

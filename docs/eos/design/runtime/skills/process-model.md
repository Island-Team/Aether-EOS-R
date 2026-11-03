# Skill process model

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Design](../../README.md) / [Runtime](../README.md) / [Skills](README.md) / **process model**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [design/](../../README.md)
      - [runtime/](../README.md)
        - [skills/](README.md)
          - [authoring.md](authoring.md)
          - [data.md](data.md)
          - [external.md](external.md)
          - [managed.md](managed.md)
          - [manager.md](manager.md)
          - [native.md](native.md)
          - [observability.md](observability.md)
          - **process-model.md**

</details>
<!-- docs-tree:end -->

[Overview](../skills.md) · [IPC](../../../interfaces/skills/native/community.md) · [Authoring](authoring.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/process-model_cn.md)

SkillManager treats each invocation as a supervised logical process. A definition is the executable-like catalog entry; an execution ID is the PID-like opaque identity. The analogy supplies ownership and inspection, not OS isolation.

## Process table

The manager creates the execution record before admission to a worker and owns it through terminal retention. A snapshot identifies the exact skill and runtime, request and parent execution, boot, lifecycle state, deadline, timestamps, claims, activities, operations, accounting, and terminal cause. Existing `Get/List/WatchExecution` APIs remain the public process-table view.

The built-in C++20 coroutine framework remains in libraries and tests, but production startup registers no built-in skill definitions. Packaged ABI v2 skills execute in one worker process per execution. The implemented primitive and timer awaitables suspend the coroutine; mailbox and child operations are bounded manager calls. Built-in timestamped source awaitables are not implemented. Blocking code outside these wrappers is an author error and cannot be safely preempted.

## Scheduling and operations

SkillManager has four workers by default, and each built-in registry has eight primitive-operation workers. Limits are 64 outstanding primitive operations per execution and 256 per registry. Each operation has an ID, dynamic device lease, cancellation, and result; Dispatcher retains final safety admission and same-device ordering.

Non-reentrancy is scoped by exact skill identity and runtime. Explicit device and resource claims remain global. Queue deadlines wake independently of worker availability, and observer callbacks use separate bounded capacity.

## Parent and child rules

`spawn` creates an ordinary execution with `parent_execution_id`. Trees have a maximum depth of 8 and 32 live children per parent. Self-spawn, ancestor cycles, unrelated-handle attachment, detached children, and claim combinations that would deadlock are rejected. Parent completion, cancellation, timeout, and shutdown request cancellation of active descendants. Child failure is returned by `await_exit`; it does not automatically choose the parent's outcome.

Built-in termination is cooperative. Packaged ABI v2 workers already provide address-space isolation and process termination. The worker applies mandatory Landlock, seccomp, `no_new_privs`, and resource limits before loading skill code. Cgroups are not part of this worker admission path.

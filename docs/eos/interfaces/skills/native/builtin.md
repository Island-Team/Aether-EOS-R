# Author a built-in C++ skill

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Interfaces](../../README.md) / [Skills](../README.md) / [Native](README.md) / **builtin**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [interfaces/](../../README.md)
      - [skills/](../README.md)
        - [native/](README.md)
          - [abi.md](abi.md)
          - [build-install-test.md](build-install-test.md)
          - **builtin.md**
          - [capabilities.md](capabilities.md)
          - [community.md](community.md)
          - [example.md](example.md)
          - [package-abi.md](package-abi.md)

</details>
<!-- docs-tree:end -->

[Native skill index](README.md) · [中文](../../../../../docs_cn/eos/interfaces/skills/native/builtin_cn.md) · [Runtime details](../../../design/runtime/skills/authoring.md)

The built-in C++ framework is retained for library development and tests. Production startup registers no built-in skill definitions; deployed skills use the packaged ABI v2 worker path. Definitions can be compiled into a test or alternate composition and explicitly registered in its island skill registry. Use `eos::runtime::skills::builtin::Definition` with an exact name and version, object-valued parameter and result schemas, declared primitive and resource capabilities, SIGNAL and mailbox channels, claims, and a `Task<Outcome>` handler. The registry rejects invalid or duplicate definitions during startup admission.

A definition in such a test composition can use the `builtin::Execution` interface. Implement the handler with the `builtin::Execution` wrappers for declared resource reads, primitive start/wait/cancel, cancellation checks, SIGNALs, parent/child mailbox messages, checkpoints, and diagnostics. Register it explicitly through that composition’s island registry; static registration is not supported.

The handler must not access Dispatcher, hardware, model internals, workflow state, the filesystem, or the network directly. Keep every operation bounded and cooperate with cancellation. Return stable reason codes with bounded operator messages. Test admission and success, rejection, cancellation, timeout, claim conflicts, mailbox backpressure, and cleanup with fake hooks before deploying the new skill.

The [runtime authoring page](../../../design/runtime/skills/authoring.md) has a minimal `Definition` example and the process model. Use [SkillService](../../skills.md) to discover and execute an admitted skill from another process.

# Native ABI v2 skill

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Interfaces](../../README.md) / [Skills](../README.md) / [Native](README.md) / **package abi v2**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [interfaces/](../../README.md)
      - [skills/](../README.md)
        - [native/](README.md)
          - [abi.md](abi.md)
          - [build-install-test.md](build-install-test.md)
          - [builtin.md](builtin.md)
          - [capabilities.md](capabilities.md)
          - [community.md](community.md)
          - [example.md](example.md)
          - **package-abi.md**

</details>
<!-- docs-tree:end -->

[简体中文](../../../../../docs_cn/eos/interfaces/skills/native/package-abi_cn.md)

[Interface overview](../../README.md) · [SkillService](../../skills.md) · [How skills work](../../../design/runtime/skills.md)

**ABI ownership and target integration:** [EOS-hosted ABI and package boundary](host-abi.md). This page is the current native authoring workflow. The [raw C layout](abi.md), [capability operations](capabilities.md), and [community contract](community.md) each own their field details; keep package usage here without redefining those contracts.

ABI v2 packaged modules are the production path in the opt-in C++ skill side chain. The target Rust EOS supervisor has not integrated that path yet. The [built-in C++ framework](../../../design/runtime/skills/authoring.md) remains available for library development and tests; C++ production startup registers no built-in definitions.

This guide is only for authors building an EOS native skill. C++17 authors should use the header-only `<eos/skill/sdk.hpp>` facade, the `EOS::SkillCpp` CMake target, and `eos_add_skill`. The stable ABI v2 in `<eos/skill/abi.h>` remains available to C authors and compatibility code. A library that exports only the former v1 query symbol is rejected during startup admission.

Native skills are reviewed C or C++ ABI v2 shared libraries. Enabled packaged libraries load in `eos-skill-worker`, one process per execution, and use EOS-brokered capabilities. The worker broker handles primitive, resource, inference, source, SIGNAL, checkpoint, and optional ABI v2.1 community calls under descriptor authority. Child-process callbacks are not bridged. Deployment review remains mandatory.

## Authoring path

1. Install the `EOSSkill` CMake package and include `<eos/skill/sdk.hpp>`.
2. Define object-valued parameter, result, and SIGNAL JSON Schemas.
3. Declare every primitive, resource URI template, SIGNAL, and whole-execution claim the skill needs.
4. Implement one bounded handler returning `eos::skill::Outcome`; use `Execution` for arguments, cancellation, capabilities, logs, and checkpoints.
5. Start from the [complete example](example.md).
6. Follow [build, install, and test](build-install-test.md).

The [capabilities guide](capabilities.md) shows how to call primitives, read resources, and report observability. The [ABI contract](abi.md) documents the underlying C boundary. Runtime scheduling, records, retention, and event delivery belong in [How skills work](../../../design/runtime/skills.md); public gRPC usage belongs in [SkillService](../../skills.md).

Deployment skills can read shared perception resources for pose, action, and identity while keeping their own source local.

## Cross-skill cooperation

A skill reads declared resources for snapshots and invokes declared primitives to change hardware or external state. It cannot invoke another skill as a nested execution, retain another execution's handle, or write through a resource API. For communication with active peers, declare a [skill community](community.md) channel and use its addressed send or topic operations.

Use declared, object-valued SIGNALs for Agent-routed coordination and control. Deployment skills can define a shared `skill.command` pause/resume input and a `skill.event` lifecycle/progress output. A skill may also define `stop` as a successful domain completion, for example. SIGNALs do not expand descriptor authority; manager `TerminateExecution` remains the canonical cancellation operation.

Prefer asynchronous primitives for interruptible work: start only the operations needed, poll them with bounded waits, and check cancellation and inbound control between waits. A current host permits up to 64 outstanding operations per invocation. Use the synchronous fallback only when an older ABI v2 host omits the optional asynchronous callbacks.

## Required conditions

- Library and skill names are lower-case identities; versions are exact SemVer.
- Use `EOS_SKILL_EXPORT_LIBRARY` to export only `eos_skill_library_query_v2` and return a valid v2 library table.
- Keep descriptors, schemas, declaration arrays, callbacks, and context alive and immutable for the process lifetime.
- Use self-contained object JSON Schemas; remote and file `$ref` values are rejected.
- Treat the host table and all borrowed slices as invocation-scoped.
- Call host functions only on the invoking thread.
- Check cooperative cancellation around every bounded unit of work.
- Let `eos::skill::Run` translate one `Outcome` into exactly-once completion.
- Give unsuccessful outcomes a stable reason code and a bounded, payload-free operator message; never make callers parse the message.
- Never retain host pointers, start a persistent worker, or let an exception cross the C ABI.

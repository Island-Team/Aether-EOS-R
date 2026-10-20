# Legacy native ABI v2 skill

[简体中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/README_cn.md)

[Interface overview](../../README.md) · [SkillService](../../skills.md) · [How skills work](../../../../design/runtime/skills.md)

> **Legacy SDK:** this guide documents ABI v2 for existing deployment-owned modules. New first-party skills should use the [built-in C++ authoring API](../../../../design/runtime/skills/authoring.md). Native ABI v2 remains supported during migration but is no longer the preferred authoring surface.

This guide is only for authors building an EOS native skill. C++17 authors should use the header-only `<eos/skill/sdk.hpp>` facade, the `EOS::SkillCpp` CMake target, and `eos_add_skill`. The stable ABI v2 in `<eos/skill/abi.h>` remains available to C authors and compatibility code. A library that exports only the former v1 query symbol is rejected during startup admission.

Native skills are reviewed C or C++ shared libraries loaded into `eosd`. They compose declared primitives, read declared resources, receive and emit declared SIGNALs, and declare scheduling claims. They are trusted in-process code, not sandboxed plugins, so deployment review is mandatory.

## Authoring path

1. Install the `EOSSkill` CMake package and include `<eos/skill/sdk.hpp>`.
2. Define object-valued parameter, result, and SIGNAL JSON Schemas.
3. Declare every primitive, resource URI template, SIGNAL, and whole-execution claim the skill needs.
4. Implement one bounded handler returning `eos::skill::Outcome`; use `Execution` for arguments, cancellation, capabilities, logs, and checkpoints.
5. Start from the [complete example](example.md).
6. Follow [build, install, and test](build-install-test.md).

The [capabilities guide](capabilities.md) shows how to call primitives, read resources, and report observability. The [ABI contract](abi.md) documents the underlying C boundary. Runtime scheduling, records, retention, and event delivery belong in [How skills work](../../../../design/runtime/skills.md); public gRPC usage belongs in [SkillService](../../skills.md).

The maintained [`observe_human` skill](../../../../../../share/skills/native/observe_human/README.md) provides a native product example: `1.0.0` is a controlled scan-and-follow process with fixed tracking constants and two deployment identifiers. It stabilizes and tracks a human without private perception inference or attachment logging. Skills that need pose, action, or identity read them independently from the shared perception resource.

## Cross-skill cooperation

A skill reads declared resources for snapshots and invokes declared primitives to change hardware or external state. It does not call another skill, retain another execution's handle, or write through a resource API.

Use declared, object-valued SIGNALs for Agent-routed coordination and control. The maintained Island skills demonstrate a shared `skill.command` pause/resume input and a `skill.event` lifecycle/progress output. A skill may also define `stop` as a successful domain completion, as `observe_human@1.0.0` does. SIGNALs do not expand descriptor authority; manager `TerminateExecution` remains the canonical cancellation operation.

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

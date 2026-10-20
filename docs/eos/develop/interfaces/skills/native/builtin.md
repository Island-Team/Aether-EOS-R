# Author a built-in C++ skill

[Native skill index](README.md) · [中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/builtin_cn.md) · [Runtime details](../../../../design/runtime/skills/authoring.md)

Built-in skills are compiled into EOS and registered explicitly in the island skill registry. Use `eos::runtime::skills::builtin::Definition` with an exact name and version, object-valued parameter and result schemas, declared primitive and resource capabilities, SIGNAL and mailbox channels, claims, and a `Task<Outcome>` handler. The registry rejects invalid or duplicate definitions during startup admission.

Start from a maintained example such as [`navigate_waypoint`](../../../../../../share/skills/native/navigate_waypoint/navigate_waypoint.cpp). Implement the handler with the `builtin::Execution` wrappers for declared resource reads, primitive start/wait/cancel, cancellation checks, SIGNALs, parent/child mailbox messages, checkpoints, and diagnostics. Register it through the island registry during normal initialization; static registration is not supported.

The handler must not access Dispatcher, hardware, model internals, workflow state, the filesystem, or the network directly. Keep every operation bounded and cooperate with cancellation. Return stable reason codes with bounded operator messages. Test admission and success, rejection, cancellation, timeout, claim conflicts, mailbox backpressure, and cleanup with fake hooks before deploying the new skill.

The [runtime authoring page](../../../../design/runtime/skills/authoring.md) has a minimal `Definition` example and the process model. Use [SkillService](../../skills.md) to discover and execute an admitted skill from another process.

# Skill runtime core contracts

This folder contains only provider-neutral contracts shared by SkillManager and concrete runtimes:

- `provider.h` defines `Provider`, metadata, capabilities, claims, SIGNALs, hooks, and `RunResult`.
- `model.*` defines execution snapshots, events, receipts, statuses, resolved claims, and configurable bounds.
- `catalog.*` copies and validates one provider's discovered metadata, compiles SIGNAL schemas, performs exact lookup, and resolves claims.

Supervisor state and behavior do not belong here. Runtime registration, workers, records, scheduling, retention, event queues, observers, and logging live in `../manager/`. ABI loading and language-specific execution live in their provider folders.

See [`docs/eos/runtime/skills.md`](../../../../../docs/eos/design/runtime/skills.md) and the [SkillManager reference](../../../../../docs/eos/design/runtime/skills/manager.md).

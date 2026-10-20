# SkillManager supervisor

This folder owns multi-runtime supervision:

- `registry.*` validates `SkillRuntime` registrations and maps each exact `{name, version}` to one `OwnedSkill`.
- `manager.*` owns the public execution lifecycle, shared workers, cancellation, deadlines, SIGNALs, observers, and shutdown.
- `scheduler.*` arbitrates global device/resource claims and runtime-scoped provider-context claims.
- `record_store.*` owns retained records and request-id indexes.
- `events.*` owns lifecycle/event append and replay plus inbound SIGNAL queues.
- `logging.*` applies the shared payload-free `skills` logging policy.
- `execution.h` keeps the compatibility `ExecutionService` alias.
- `tests/` verifies generic and multi-runtime behavior.

Package loading and provider-specific execution do not belong here. See [`docs/eos/runtime/skills/manager.md`](../../../../../docs/eos/design/runtime/skills/manager.md) for the complete contract.

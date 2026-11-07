# `egia / cognition / decision / planner`

[中文](../../../../../../../docs_cn/ei/layers/cognition/groups/decision/nodes/planner.md) · [Stage contract](../../../../../interfaces/pipeline.md) · [Current module algorithm](../../../../../core/decision.md)

**Target type:** `egia.decision.planner@v0.1`. **Type description:** Uses configured model lanes to propose typed primitive steps, without execution authority. One configured instance is one observable stage in the Python process, not a child process. The instance has its own nonblank operator description. A01 binds this graph instance to the existing Python owner in a managed EGIA launch. The typed link → perception → context → decision evidence path is exercised with stub EOS; mission, result, health, and governed command behavior still follows the existing core module until the remaining stage integration slices.

**Inputs:** selected, snapshot. **Outputs:** proposal, plan_state. The [stage contract](../../../../../interfaces/pipeline.md) is authoritative for port value types, units, optionality, defaults, setting validation and compatibility; the graph contract owns wire priority and frequency.

**Algorithm and failure:** Bind the selected mission to a bounded context and model lane; validate structured output against the current plan. A model timeout, malformed reply, or canceled call yields failure or retry policy, never an admitted action.

**Compatibility:** This `@v0.1` stage is a new identity, with no one-to-one `@v1.0` predecessor. The known historical `egia.common.source/context/planner/guard@v1.0` design identities retain only their established, non-executing behavior and are never silently remapped to this stage. New optional settings require defaults; a changed port meaning, unit, direction, safety rule or action authority requires a new type ID. Unknown future type versions fail with a node-path diagnostic.

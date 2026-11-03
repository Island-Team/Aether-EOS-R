# Legacy primitive control node

[中文](../../../../../../../docs_cn/eos/layers/control/groups/primitive/nodes/primitive_cn.md) · [EOS layer catalog](../../../../README.md) · [control](../../../../../interfaces/control.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md)

**Placement:** `eos/control/primitive/<instance-id>` · **Type family:** `eos.control.primitive`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Compatibility only for imported graphs. New graphs use `eos.dispatcher.primitive_base@v0.1` or `eos.dispatcher.primitive_manipulator@v0.1` after implementation. Current identity: `eos.control.primitive@v1.0`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

Legacy node selects one target and movement mode in settings. New binding nodes place target identity in a typed graph edge.

## Ports and settings

Required settings: `target` and `movement_type`. Inputs: dispatcher `status` and one matching observation. Outputs: `request: ControlRequest`, `outcome: CommandOutcome`. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

For an imported graph, resolve configured target and mode, check references and input types, then submit through Dispatcher. Migration to binding nodes requires an explicit revision-checked graph write; reading alone preserves saved bytes. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

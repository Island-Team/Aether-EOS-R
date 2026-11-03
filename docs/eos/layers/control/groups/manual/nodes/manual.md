# Manual control node

[中文](../../../../../../../docs_cn/eos/layers/control/groups/manual/nodes/manual_cn.md) · [EOS layer catalog](../../../../README.md) · [control](../../../../../interfaces/control.md) · [hardware](../../../../../interfaces/hardware.md)

**Placement:** `eos/control/manual/<instance-id>` · **Type family:** `eos.control.manual`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Current legacy graph type retained for safe reads; target UI controls use admitted EOS requests. Current identity: `eos.control.manual@v1.0`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

Represents a user-selected base or arm target and exposes request outcome. It does not own an actuator or a second command route.

## Ports and settings

Required setting: `target` canonical hardware path. Inputs: dispatcher `status` plus exactly one matching base or arm observation. Outputs: `request: ControlRequest`, `outcome: CommandOutcome`. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Resolve the admitted target from graph state, validate the user movement against its type and limits, then submit through Dispatcher. `manual_stop` stops that configured target; a caller cannot substitute a different path. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

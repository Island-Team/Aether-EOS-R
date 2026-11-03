# Mobile base adapter node

[中文](../../../../../../../docs_cn/eos/layers/hardware/groups/base/nodes/base_cn.md) · [EOS layer catalog](../../../../README.md) · [hardware](../../../../../interfaces/hardware.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md) · [mujoco](../../../../../interfaces/mujoco.md)

**Placement:** `eos/hardware/base/<instance-id>` · **Type family:** `eos.hardware.base`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.hardware.base@v1.0` and `eos.hardware.base@v0.1` are registered; new graphs use `@v0.1`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

One base owns observations and the physical stop seam. An input edge describes the command route but cannot authorize direct actuation.

## Ports and settings

Input: `command: BaseMovement` (many compatible sources). Outputs: `observation: BaseObservation`, `outcome: CommandOutcome`, `health: Health`, `status: Status`. Modes: `body_velocity` and/or `relative_pose` only when advertised. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Resolve the verified adapter, validate command source and typed limits, ask Dispatcher to admit, then release to the one adapter. Lease expiry or stop calls the adapter stop seam; uncertain completion remains `OutcomeUnknown` and is never retried automatically. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

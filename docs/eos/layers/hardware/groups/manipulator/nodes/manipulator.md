# Manipulator adapter node

[中文](../../../../../../../docs_cn/eos/layers/hardware/groups/manipulator/nodes/manipulator_cn.md) · [EOS layer catalog](../../../../README.md) · [hardware](../../../../../interfaces/hardware.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md) · [mujoco](../../../../../interfaces/mujoco.md)

**Placement:** `eos/hardware/manipulator/<instance-id>` · **Type family:** `eos.hardware.manipulator`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.hardware.manipulator@v1.0` and `eos.hardware.manipulator@v0.1` are registered; new graphs use `@v0.1`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

One arm owns ordered joint state, optional tool pose, and its physical stop seam. Node profile joint order is authoritative.

## Ports and settings

Input: `command: ManipulatorMovement` (many compatible sources). Outputs: `observation: ManipulatorObservation`, `outcome: CommandOutcome`, `health: Health`, `status: Status`. Modes: `joint_positions` and/or `end_effector_pose` only when advertised. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Validate joint names, order, finite targets, mode and bounds before Dispatcher admission. The adapter reports fresh observed state; an acknowledgment alone does not prove arrival. Stop or uncertain outcome cannot be hidden by a graph edge. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

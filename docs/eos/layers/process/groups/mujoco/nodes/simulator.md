# MuJoCo simulator process node

[中文](../../../../../../../docs_cn/eos/layers/process/groups/mujoco/nodes/simulator_cn.md) · [EOS layer catalog](../../../../README.md) · [node-contributions](../../../../../interfaces/node-contributions.md) · [mujoco](../../../../../interfaces/mujoco.md) · [hardware](../../../../../interfaces/hardware.md)

**Placement:** `eos/process/mujoco/<instance-id>` · **Type family:** `eos.process.mujoco`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Package-contributed `@v1.0` exists in current sidecars; new-graph `@v0.1` is target design. The old built-in `eos.process.backend@v1.0` remains safe-read compatibility. Current identity: `eos.process.mujoco@v1.0`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

IsOS verifies package `island/mujoco-sim@0.1.1`, resolves scene and executable, and applies the graph. EOS supervises the simulator as its direct child and owns the internal socket adapter.

## Ports and settings

Package pin: exact `node.package`. Settings: `scene`, `socket`, optional `env` and `start_timeout_ms`. Outputs: `status: Status`, `health: Health`. Hardware adapters reference this process and the same verified socket/device IDs. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Validate package and scene, spawn one child, wait for owner-only socket readiness, then admit declared device adapters. On stop, close admission and stop actuators before terminating and reaping the simulator. A failed stop remains visible. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

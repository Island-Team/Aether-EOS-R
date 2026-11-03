# Shared control dispatcher node

[中文](../../../../../../../docs_cn/eos/layers/dispatcher/groups/control/nodes/dispatcher_cn.md) · [EOS layer catalog](../../../../README.md) · [control](../../../../../interfaces/control.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md)

**Placement:** `eos/dispatcher/control/<instance-id>` · **Type family:** `eos.dispatcher.control`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Current `@v1.0` identity remains for compatibility. New graphs use `eos.dispatcher.control@v0.1` to represent the shared admission owner; primitive bindings do not create another dispatcher. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

The single actuator admission owner checks identity, health, freshness, deadlines, limits and safety before any backend release. A graph node represents this owner and its typed route; it does not create a second dispatcher.

## Ports and settings

Input: `request: ControlRequest`. Outputs: `base_command: BaseMovement`, `manipulator_command: ManipulatorMovement`, `status: Status`. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Normalize a request, reject invalid or stale work, reserve command identity, record admission, release once, then observe or stop. Priority on a routed input only schedules delivery; it never overrides these checks. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

# Manipulator primitive binding node

[中文](../../../../../../../docs_cn/eos/layers/dispatcher/groups/manipulator/nodes/primitive-manipulator_cn.md) · [EOS layer catalog](../../../../README.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md) · [hardware](../../../../../interfaces/hardware.md)

**Placement:** `eos/dispatcher/manipulator/<instance-id>` · **Type family:** `eos.dispatcher.primitive_manipulator`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.dispatcher.primitive_manipulator@v1.0` and `eos.dispatcher.primitive_manipulator@v0.1` are registered; new graphs use `@v0.1`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

The node names one versioned arm operation and one explicit edge to a compatible manipulator; the hardware adapter still owns the device.

In the planned IsosUI EOS panel, this **active instance** appears as one selectable primitive control. The panel shows its type and instance descriptions, single target, declared movement mode and EOS live health; an invocation uses the existing `primitive_typed` bridge and displays the correlated result. The graph edge is a binding, not an executable call.

## Ports and settings

Settings: `primitive_name`, `primitive_version`, `movement_type`. Output: `command: ManipulatorMovement`; no input. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Validate the binding catalog, edge target, joint order and typed command header, then ask Dispatcher to admit. A failed or uncertain arm motion stops via the owning adapter. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

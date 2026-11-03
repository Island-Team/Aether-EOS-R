# Base primitive binding node

[中文](../../../../../../../docs_cn/eos/layers/dispatcher/groups/base/nodes/primitive-base_cn.md) · [EOS layer catalog](../../../../README.md) · [primitive-bindings](../../../../../interfaces/primitive-bindings.md) · [hardware](../../../../../interfaces/hardware.md)

**Placement:** `eos/dispatcher/base/<instance-id>` · **Type family:** `eos.dispatcher.primitive_base`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.dispatcher.primitive_base@v1.0` and `eos.dispatcher.primitive_base@v0.1` are registered; new graphs use `@v0.1`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

The node names one versioned base operation and one graph edge to a compatible base device; it owns no adapter and no admission bypass.

In the planned IsosUI EOS panel, this **active instance** appears as one selectable primitive control. The panel shows its type and instance descriptions, single target, declared movement mode and EOS live health; an invocation uses the existing `primitive_typed` bridge and displays the correlated result. The graph edge is a binding, not an executable call.

## Ports and settings

Settings: `primitive_name`, `primitive_version`, `movement_type`. Output: `command: BaseMovement`; no input. The device `inputs.command` edge identifies the target. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Resolve exactly one compatible device from the explicit edge, compare name/version/mode with the admitted primitive catalog, then pass a typed command header through Dispatcher. Reject ambiguity before release. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

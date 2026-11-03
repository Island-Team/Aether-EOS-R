# Camera adapter node

[中文](../../../../../../../docs_cn/eos/layers/hardware/groups/camera/nodes/camera_cn.md) · [EOS layer catalog](../../../../README.md) · [hardware](../../../../../interfaces/hardware.md) · [vision](../../../../../interfaces/vision.md) · [mujoco](../../../../../interfaces/mujoco.md)

**Placement:** `eos/hardware/camera/<instance-id>` · **Type family:** `eos.hardware.camera`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.hardware.camera@v1.0` and `eos.hardware.camera@v0.1` are registered; new graphs use `@v0.1`. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

One configured camera owns one adapter and produces typed frames, health, and lifecycle status. RGB is required when advertised; aligned depth is optional and must be reported unavailable when absent.

## Ports and settings

Inputs: none. Outputs: `frame: CameraFrame`, `health: Health`, `status: Status`. Capabilities: `rgb8`, optional `aligned_depth_u16`. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Validate the node profile and adapter family, construct exactly one device adapter, sample into the bounded latest-value hub, then publish only validated fresh frames. A bad frame degrades this camera alone; it never fabricates a replacement frame. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

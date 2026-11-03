# Camera preview gate node

[中文](../../../../../../../docs_cn/eos/layers/interface/groups/gate/nodes/camera-gate_cn.md) · [EOS layer catalog](../../../../README.md) · [vision](../../../../../interfaces/vision.md) · [hardware](../../../../../interfaces/hardware.md)

**Placement:** `eos/interface/gate/<instance-id>` · **Type family:** `eos.interface.camera_gate`. The registered type is a reusable definition; the deployment chooses each instance ID and its own nonblank instance description.

**Status:** Both `eos.interface.camera_gate@v1.0` and `eos.interface.camera_gate@v0.1` are registered; new graphs use `@v0.1`. Preview route support depends on runtime admission. Never rewrite an imported graph merely by reading it.

## Purpose and boundary

Transforms an admitted camera frame into a bounded read-only preview for IsosUI Vision. It cannot send camera control or actuator commands.

## Ports and settings

Input: `frame: CameraFrame` (image, in-process). Output: `preview: VisionFrame` (image, WebSocket). No node-specific settings. The linked interface pages own exact field types, units, defaults, validation, examples, and compatibility rules. New type definitions require a nonblank registered `NodeType.description`; old sidecars receive the documented compatibility fallback.

## Lifecycle and algorithm

Bind one compatible camera source, validate and bound frame dimensions/age, encode or forward the latest permitted preview, and drop stale frames under backpressure. Viewer disconnect leaves camera acquisition and safety unaffected. Node state, rejection reason, and correlation IDs appear in the shared deployment log.

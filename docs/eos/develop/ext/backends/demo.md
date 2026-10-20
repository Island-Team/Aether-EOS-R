# Demo backend

[Chinese](../../../../../docs_cn/eos/develop/ext/backends/demo_cn.md)

The demo backend is an EOS adapter for the separately launched `eos-simulator` process. It is not an EOS-core mode, an in-process world, a physics engine, or a safety qualification environment. Admission, dispatch, feeds, models, and northbound services use the same EOS paths as other backends.

## Isolation and availability

All demo adapters share a bounded client for `<run>/simulator.sock`. They use the private `eos.simulator.v1.SimulatorService` methods `Health`, `Execute`, `ReadTelemetry`, and `ReadCameraFrame`; they never construct, start, or tick a world. `eos-simulator` owns its monotonic clock and state.

If the simulator is missing or disconnects, adapters report unhealthy, reject commands, and return no camera frame. The client reconnects on later calls. `eosd` stays alive, does not supervise the simulator, and keeps non-demo functionality available.

## Device behavior

- **base** integrates velocity and applies navigation against the simulator's raster collision map.
- **gimbal**, **arm**, and **gripper** update their simulated telemetry and state.
- **cam** reads a bounded JPEG and the exact base/gimbal/scene association from the simulator, then follows the normal EOS perception-model path.
- **mic** and **speaker** retain the configured host ALSA and native `aplay` behavior; simulator media is not an audio-input shortcut.

The simulator uses a right-handed planar frame: positive X points right, positive Y points up, zero yaw points along positive X, and positive yaw is counter-clockwise. Its raster is the collision authority. Vision source lines are triggers only and never become collision geometry, labels, detections, or pick targets.

With simulator `camera_source = "scene"`, the simulator casts one horizontal ray at `base.yaw + gimbal.pan`, chooses the nearest positive source-line hit, resolves its `source_id` through the global source library, and lazily reads/decodes that canonical file. A miss or unavailable/invalid source yields no frame. EOS independently validates model output; map metadata is never a fallback detection.

With `camera_source = "system"`, the simulator instead captures the operating system's default camera as the only source; map source lines are ignored. This still reaches EOS through the `demo` camera and simulator IPC. The separate `demo-host` profile bypasses the simulator camera path and captures `/dev/video0` directly in `eosd` via V4L2.

## Profiles and operation

`etc/eos/backends/demo.toml` defines the demo devices. The `demo-host` variant retains the same robot contract but replaces the simulator camera with V4L2 capture. Profile limits are configuration examples, not hardware safety limits.

Launch `eos-simulator` explicitly before or after `eosd`; neither process controls the other's lifetime. Its configured `map_id` is loaded from `<state>/simulator/maps`. Changing that ID requires simulator restart, while a committed edit to the active map is hot-reloaded atomically. The WebUI talks directly to the simulator on loopback port `16004`; EOS northbound does not proxy simulator routes.

## Diagnostics

Simulator lifecycle, IPC recovery, source selection, commits, hot reloads, and bounded failures use the shared structured logging policy. Normal ticks/frames and payload data are not logged, and repeated disconnect/decode warnings are rate-limited. See the [standalone simulator interface](../../interfaces/external/simulator.md) for persistence, HTTP, WebSocket, validation, and logging details.

Tests must cover process isolation, disconnect/reconnect, command and telemetry bounds, camera absence during failure, nearest-line resolution, lazy source access, and normal model processing after reconnection.

**Related:** [simulator interface](../../interfaces/external/simulator.md) · [southbound interface](../../interfaces/southbound/primitives.md) · [hardware runtime](../../../design/runtime/hardware.md)

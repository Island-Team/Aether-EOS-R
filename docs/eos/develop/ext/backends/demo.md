# Demo backend

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Develop](../../README.md) / [Ext](../README.md) / [Backends](README.md) / **demo**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [develop/](../../README.md)
      - [ext/](../README.md)
        - [backends/](README.md)
          - **demo.md**
          - [host.md](host.md)
          - [ros2.md](ros2.md)

</details>
<!-- docs-tree:end -->

[Chinese](../../../../../docs_cn/eos/develop/ext/backends/demo_cn.md)

This page describes the opt-in C++ demo adapter and its `eos.simulator.v1` service. The default Rust v2 MuJoCo path uses the [v2 interface](../../../interfaces/mujoco.md), where EOS supervises the verified simulator as a direct child. The C++ adapter is an EOS client for the external simulator service; EOS does not include a simulator world, physics engine, or safety qualification environment.

## Isolation and availability

All demo adapters share a bounded client for `<run>/simulator.sock`. They use the private `eos.simulator.v1.SimulatorService` methods `Health`, `Execute`, `ReadTelemetry`, and `ReadCameraFrame`; they never construct, start, or tick a world. The external simulator process owns its clock and state.

If the simulator is missing or disconnects, adapters report unhealthy, reject commands, and return no camera frame. The client reconnects on later calls. `eosd` stays alive, does not supervise the simulator, and keeps non-demo functionality available.

## Device behavior

- **base** integrates velocity and applies navigation against the simulator's raster collision map.
- **gimbal**, **arm**, and **gripper** update their simulated telemetry and state.
- **cam** reads a bounded JPEG and the exact base/gimbal/scene association from the simulator, then follows the normal EOS perception-model path.
- **mic** and **speaker** use loopback stubs in the default `demo` profile. The opt-in `demo-host` profile uses host ALSA capture and native `aplay` playback.

The simulator uses a right-handed planar frame: positive X points right, positive Y points up, zero yaw points along positive X, and positive yaw is counter-clockwise. Its raster is the collision authority. Vision source lines are triggers only and never become collision geometry, labels, detections, or pick targets.

With simulator `camera_source = "scene"`, the simulator casts one horizontal ray at `base.yaw + gimbal.pan`, chooses the nearest positive source-line hit, resolves its `source_id` through the global source library, and lazily reads/decodes that canonical file. A miss or unavailable/invalid source yields no frame. EOS independently validates model output; map metadata is never a fallback detection.

With `camera_source = "system"`, the simulator instead captures the operating system's default camera as the only source; map source lines are ignored. This still reaches EOS through the `demo` camera and simulator IPC. The separate `demo-host` profile bypasses the simulator camera path and captures `/dev/video0` directly in `eosd` via V4L2.

## Profiles and operation

`etc/eos/backends/demo.toml` defines the demo devices. The `demo-host` variant retains the same robot contract but selects V4L2 camera capture, ALSA microphone capture, and native `aplay` playback. Profile limits are configuration examples, not hardware safety limits.

In the historical C++ stack, the launcher starts the selected simulator package before EOS. In v2, EOS directly starts and stops its simulator child. See the [MuJoCo usage guide](../../../usage/mujoco.md) for current package configuration and readiness.

## Diagnostics

Simulator lifecycle, IPC recovery, source selection, commits, hot reloads, and bounded failures use the shared structured logging policy. Normal ticks/frames and payload data are not logged, and repeated disconnect/decode warnings are rate-limited. See the [standalone simulator interface](../../../interfaces/external/simulator.md) for persistence, HTTP, WebSocket, validation, and logging details.

Tests must cover process isolation, disconnect/reconnect, command and telemetry bounds, camera absence during failure, nearest-line resolution, lazy source access, and normal model processing after reconnection.

**Related:** [simulator interface](../../../interfaces/external/simulator.md) · [southbound interface](../../../interfaces/southbound/primitives.md) · [hardware runtime](../../../design/runtime/hardware-legacy.md)

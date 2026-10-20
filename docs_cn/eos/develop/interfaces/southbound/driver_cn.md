# Southbound Interface

[Interface overview](../README_cn.md) links the contract surfaces and runtime owners.

This is the driver-author contract between EOS and one admitted backend, declared by `eos/hardware/backend.h`. It is a build-time C++ interface, not part of the Agent-facing wire API.

## Driver responsibilities

EOS owns binding admission, schema validation, targeting, serialization, registry-resolved command ranges, safety gating, and lifecycle. A driver owns hardware communication, kinematics, trajectory generation, authoritative device limits, collision protection, and feed production. It must return `ReleaseResult::kRejected` for a command it cannot safely realize.

The bridge assigns the generated `<class>-<index>` device ID, calls `Identify()` and `DeclareFeeds()`, checks declared feed rates, and admits the backend only when those checks pass. Dispatcher is the sole mutable release caller; it serializes release calls, while the gateway core serializes bridge `Tick()` with northbound resource projection.

## Common interface rules

- `BackendIdentity` reports `name`, `version`, generated `device_id`, and admitted `DeviceKind`.
- `DeclareFeeds()` reports maximum rates for `geometric`, `proprioception`, `model`, and `health`; exceeding a binding limit fails admission.
- `ReadHealth()` returns backend-neutral `{alive, admitted}` state.
- Every release returns `kAccepted`, `kRejected`, or `kTimedOut` and carries a release-time `steady_clock` stamp.
- Optional operation methods reject by default. Feed-only camera and microphone drivers therefore implement no command path.

Commands use explicit SI units: metres, metres per second, radians, radians per second, newtons, and milliseconds. Omitted optional speed or force selects the driver default. Caller-visible hardware ranges are mirrored into the binding primitive declaration, validated at registry admission, and enforced by dispatch; the backend still applies its authoritative device and URDF limits.

Northbound reference mode 在 Dispatcher 截止。Relative arm 与 gimbal target 会根据 fresh telemetry 解析，因此 driver 始终收到完整 absolute target。Absolute base navigation 会转换成 backend 的 robot-relative navigation command，因为这是 portable base HAL operation。

Reference-aware target command 还携带 `stream`，默认值为 `false`。普通 motion 返回一个终态结果；stream motion 在发布后立即确认，并允许 adapter 合并后续目标，同时保留 stop 与 estop。Backend 必须显式实现物理连续性；该字段不允许普通 blocking move 任意重叠。

> **Driver note:** Never silently reinterpret units, joint order, or quaternion order. Reject invalid or unsafe inputs.

## Commands by device kind

### Arm (`DeviceKind::kManipulator`)

Arms expose only two general motion operations:

- `ExecuteMoveJ(const MoveJCommand&)` receives the complete ordered `joint_positions_rad` vector plus optional positive `speed_rad_s`.
- `ExecuteMoveL(const MoveLCommand&)` receives a base-frame XYZ position in metres, a unit XYZW quaternion, and optional positive `speed_m_s`.

The `MoveJCommand` vector must match the device's configured `joint_names` count and order. Both `arm.movej` and the argument-free `arm.init` primitive release this command; `arm.init` supplies the deployment's configured `initial_joint_positions_rad`. There is no portable single-joint mask, joint-velocity stream, end-effector routing flag, Cartesian velocity, or wrench command. The driver resolves physical limits, IK for `movel`, trajectory shape, and collision safety.

Telemetry uses `ReadManipulatorTelemetry()`: physical joint positions and velocities plus end-effector position and quaternion when available. Multi-step behavior belongs in Agent/deployment-owned skills, not in a backend command.

#### ROS 2 arm mapping

The ROS 2 backend publishes both commands as `std_msgs/Float64MultiArray` on endpoints configured under `[device.backend.ros2]`:

- `movej`: `[joint_0_rad, ..., joint_n_rad, speed_rad_s]`, following `joint_names` exactly.
- `movel`: `[x_m, y_m, z_m, qx, qy, qz, qw, speed_m_s]`.

The trailing speed is `-1.0` when the northbound field is omitted, selecting the graph-edge adapter's default. Any supplied speed must be positive. The adapter rejects wrong array lengths, non-finite values, angles outside its authoritative range, and non-unit quaternions, then applies URDF/controller limits before issuing physical commands.

Telemetry remains `sensor_msgs/JointState` for joint state and `geometry_msgs/PoseStamped` for the end-effector pose. `stop` and `estop` use `std_msgs/Empty`. Vendor-specific protocols remain outside EOS in the deployment-owned graph-edge adapter.

The ROS 2 gripper command topic uses `std_msgs/Float64MultiArray` as `[action, force_n]`, where action is `0` for open or `1` for close and `-1` force selects the adapter default. Its telemetry is `[position_pct, state, force_n]`; state is `0` unknown, `1` open, `2` closed, or `3` holding, and `-1` force means the hardware has no calibrated force measurement. A ROS 2 camera subscribes to bounded JPEG `sensor_msgs/CompressedImage` samples. Deployment-owned graph-edge adapters map vendor protocols onto these reusable messages.

An optional `health` endpoint (`std_msgs/Bool`) adds adapter readiness and service-failure feedback to telemetry freshness. Arm headers must match the configured base frame for poses; arm and camera source stamps must be recent. `ReadCameraTelemetry()` exports the source publication timestamp, not a guaranteed hardware exposure time. Backend acceptance confirms local publication, not completed physical motion.

### Base (`DeviceKind::kBase`)

`ExecuteBase(const BaseCommand&)` receives body-frame `linear_x_m_s`, `linear_y_m_s`, and `angular_z_rad_s`. `ExecuteNavigateRelative(const BaseNavigateRelativeCommand&)` receives one robot-relative `x_m`, `y_m`, and `yaw_rad` goal plus `stream`. Normal mode returns only after the backend has a final result; stream mode acknowledges release according to the backend's supported streaming semantics. `ExecutePosture(BasePosture)` receives `kStand`, `kWalk`, or `kLie`; unsupported posture or navigation mechanisms reject. Proprioception reports physical odometry, velocity, and posture.

### Gimbal (`DeviceKind::kGimbal`)

`ExecuteGimbal(const GimbalCommand&)` receives absolute `pan_rad` and `tilt_rad`, with zero as center, plus optional positive `speed_rad_s`. The driver owns stabilization and physical axis limits. Proprioception reports physical pan/tilt state.

### Gripper (`DeviceKind::kGripper`)

`ExecuteGripper(const GripperCommand&)` receives `kOpen` or `kClose` and optional positive `force_n`. The driver owns device limits and grasp logic. Proprioception reports physical jaw state, applied force, and position when available；held-object association 属于 EOS perception，不属于 hardware telemetry。

### Speaker (`DeviceKind::kSpeaker`)

`ExecuteSpeaker(const SpeakerCommand&)` 接收一个完整 encoded audio asset：EOS-owned `asset_id`、当前固定为 `audio/wav` 的 media type、全部 bytes 和 release stamp。`asset_id` 仅供诊断，不是 backend 解析的路径。URI resolution、TTS、size enforcement 与第一次 16 kHz mono PCM WAV validation 在 Dispatcher 内完成；backend 要么接受整个 atomic asset，要么拒绝，不存在 HAL chunk protocol。

Native `aplay` backend 再次校验 RIFF/WAVE 长度、uncompressed PCM format、非空 data、rate/alignment、chunk boundary 与完整 frame，然后通过 standard input 直接执行 `aplay -q -D DEVICE`；不经过 shell 或 ROS 2 speaker interface。Speaker telemetry 包含 playback state、可选 asset ID、排队/已播放时长和 underrun。详见 [Aplay 扬声器后端](../../ext/backends/host_cn.md#aplay-扬声器后端)。

### Camera and microphone

`DeviceKind::kCamera` and `DeviceKind::kMicrophone` are feed-only. Their model feeds carry bounded RGB or PCM data; command releases reject. Each exposes a backend-neutral telemetry read: `ReadCameraTelemetry()` returns one bounded JPEG frame with its geometry and source stamp, and `ReadMicrophoneTelemetry()` returns one bounded signed 16-bit PCM window with its sample rate, channel count, and source stamp. An absent result means no valid sample is available — a driver with no usable device returns absence rather than an empty window, so the northbound snapshot never claims a capture that is not happening.

## Lifecycle and safety

`Tick(double dt_s)` optionally advances backend integration and is never called concurrently with command execution. Every backend implements:

- `Clamp()` to stop velocity and hold the last safe position within actuator limits;
- `Halt()` to enter the safe envelope within the declared bound.

The driver enforces the device-level safe envelope. EOS aggregates health, applies the base validity lease, routes targeted `motion.stop`, and latches process-wide `safety.estop` until restart.

## Minimal driver checklist

1. Implement identity, feed declarations, health, `Clamp()`, and `Halt()`.
2. Implement only the general command methods supported by the admitted device kind; leave all others rejecting.
3. Validate vector sizes, finite values, physical limits, and configured joint order.
4. Reject unreachable, unsafe, or unsupported targets rather than guessing.
5. Keep command execution serialized with `Tick()` and publish telemetry in physical SI units.
6. Test boundary values, omitted speed/force defaults, stop/estop, timeouts, and device loss.

## Related

- [`backend.h`](../../../../../src/eos/include/eos/hardware/backend.h) — normative C++ interface
- [Primitive payloads](../actions-payloads_cn.md) — northbound SI values
- [Bridge](../../../design/runtime/hardware_cn.md) — registry ownership and admission
- [Configuration](../../../usage/configuration_cn.md) — device declarations and backend settings

# Hardware primitive interface

[简体中文](../../../../../docs_cn/eos/develop/interfaces/southbound/primitives_cn.md)

[Interface overview](../README.md) · [Hardware body](body.md) · [World inputs](world.md)

The southbound primitive interface transfers an already validated operation from EOS to a hardware driver. These JSON blocks show the logical payload of each driver operation. Every cancellable motion carries the northbound `command_id`; continuous and target commands also carry a monotonic release stamp. Reference-aware target commands additionally carry `stream`, which defaults to false. A normal motion returns one terminal outcome. A stream motion acknowledges publication immediately and lets the adapter coalesce later targets while preserving stop and estop. Backends must durably deduplicate IDs and must never retry or re-actuate a repeated command. For non-motion operations, Dispatcher may use fresh backend telemetry to extend an accepted release to the northbound terminal boundary; speaker playback is one such case.

Northbound reference modes stop at Dispatcher. Relative arm and gimbal targets are resolved against fresh telemetry before release, so their drivers always receive complete absolute targets. Absolute base navigation is resolved into the backend's robot-relative navigation command because that is the portable base HAL operation.

## Arm joint command

`arm.init` and `arm.movej` both reach the driver as a complete joint target.

```json
{
  "joint_positions_rad": [0.0, -0.4, 0.6, 0.0, -0.3, 0.7],
  "speed_rad_s": 0.25,
  "stream": false,
  "command_id": "movej-17",
  "stamp_monotonic_ns": 8412345000000
}
```

## Arm Cartesian command

```json
{
  "position_m": [0.45, 0.0, 0.25],
  "orientation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "speed_m_s": 0.25,
  "stream": false,
  "command_id": "movel-18",
  "stamp_monotonic_ns": 8412345100000
}
```

## Arm pick selection

```json
{
  "object_id": "obj:part-7",
  "command_id": "pick-19",
  "stamp_monotonic_ns": 8412345150000
}
```

This command is a semantic selection boundary, not a grasp plan. The ROS 2 graph-edge node resolves the current object state and owns planning and physical completion. The backend does not return until the planner reports a terminal result or the configured timeout expires.

## Base velocity command

The northbound lease is enforced by EOS. The driver receives the velocity and release stamp.

```json
{
  "linear_x_m_s": 0.2,
  "linear_y_m_s": 0.0,
  "angular_z_rad_s": 0.1,
  "command_id": "velocity-42",
  "stamp_monotonic_ns": 8412345200000
}
```

## Relative navigation command

```json
{
  "x_m": 1.0,
  "y_m": -0.2,
  "yaw_rad": 0.52,
  "stream": false,
  "command_id": "nav-9",
  "stamp_monotonic_ns": 8412345300000
}
```

## Base posture command

```json
{
  "posture": "stand",
  "command_id": "posture-4"
}
```

The other valid values are `walk` and `lie`.

## Gimbal command

```json
{
  "pan_rad": 0.3,
  "tilt_rad": -0.15,
  "speed_rad_s": 0.5,
  "stream": true,
  "command_id": "gimbal-6",
  "stamp_monotonic_ns": 8412345400000
}
```

## Gripper command

```json
{
  "action": "close",
  "force_n": 6.0,
  "command_id": "grip-4",
  "stamp_monotonic_ns": 8412345500000
}
```

The other valid action is `open`. The optional force field is `null` when the backend should use its admitted default.

## Speaker command

```json
{
  "asset_id": "asset://alerts/ready.wav",
  "media_type": "audio/wav",
  "data_b64": "UklGRiQAAABXQVZFZm10IBAAAAABAAEA...",
  "stamp_monotonic_ns": 8412345600000
}
```

The driver receives one complete decoded audio asset at the native C++ boundary; `data_b64` is the JSON illustration of that byte vector. `asset_id` is an EOS-owned diagnostic identity, not a path the backend resolves. `media_type` is currently only `audio/wav`. URI resolution, TTS, size enforcement, and the first PCM WAV validation happen in Dispatcher; the backend either accepts the whole atomic asset or rejects it. No streamed chunk protocol crosses the HAL.

## Stop motion

```json
{
  "operation": "stop_motion"
}
```

## Clamp

```json
{
  "operation": "clamp",
  "cause": "velocity_lease_expired"
}
```

Clamp is the backend's immediate, best-effort transition to its safe envelope: zero velocity for moving axes and hold the last position within torque limits. EOS invokes it across admitted devices during shutdown and safety handling, including devices that do not implement the ordinary `stop_motion` primitive. For feed-only hardware such as a camera or microphone it is an explicit no-op, because there is no actuator to make safer. Unlike halt, clamp does not latch the backend against later commands.

`cause` is EOS runtime context recorded in safety logs; the native `Backend::Clamp()` method is parameterless.

## Halt

```json
{
  "operation": "halt",
  "cause": "emergency_stop"
}
```

Halt reaches the same safe envelope and latches the backend so that later commands are rejected. `stop_motion`, by contrast, is an ordinary command for one motion device and does not define process-wide safety behavior.

As with clamp, `cause` is runtime context rather than a native method argument; the HAL entry point is `Backend::Halt()`.

## Driver result

```json
{
  "result": "accepted",
  "failure_reason": null
}
```

```json
{
  "result": "failed",
  "failure_reason": "controller rejected the planned trajectory"
}
```

```json
{
  "result": "timed_out",
  "failure_reason": "controller did not reach the required boundary before the deadline"
}
```

The backend result set is `accepted`, `rejected`, `failed`, `cancelled`, `timed_out`, and `outcome_unknown`. `rejected` means the backend did not actuate; the other non-acceptance states may follow admission. Dispatcher converts the documented backend boundary and any required telemetry convergence into the northbound terminal statuses. Drivers must reject invalid, unavailable, out-of-range, or unsafe operations and must not silently reinterpret units, frames, joint order, media formats, or missing values.

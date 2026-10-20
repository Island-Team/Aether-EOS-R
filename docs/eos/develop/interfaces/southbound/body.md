# Hardware body interface

[简体中文](../../../../../docs_cn/eos/develop/interfaces/southbound/body_cn.md)

[Interface overview](../README.md) · [World inputs](world.md) · [Driver primitives](primitives.md)

The southbound boundary is a native typed C++ HAL, not an EOS↔EGIA JSON contract. Body is the complete status surface for the robot hardware: it describes each driver, whether it can participate, and the latest state of its actuators. Raw sensor observations belong to the [world facet](world.md), and commands belong to the [primitive facet](primitives.md).

The JSON blocks below are logical illustrations of the C++ values.

## Identity

```json
{
  "name": "demo-camera",
  "version": "1.0.0",
  "device_id": "camera-0",
  "device_kind": "camera"
}
```

## Feed declarations

```json
{
  "geometric": {"max_rate_hz": 10.0},
  "proprioception": {"max_rate_hz": 50.0},
  "sensor": {"max_rate_hz": 30.0},
  "health": {"max_rate_hz": 2.0}
}
```

A zero rate means the driver does not produce that feed class. `device_kind` is `manipulator`, `base`, `gimbal`, `camera`, `microphone`, `gripper`, or `speaker`.

## Health

```json
{"alive": true, "admitted": true}
```

`alive` reports the backend safe-envelope latch. `admitted` says that EOS accepted the backend identity, configuration, and feed declarations. A backend constructed outside the bridge reports `admitted: false`; the bridge sets it only after all admission checks pass. These flags describe driver availability; they do not replace the per-device state below.

## Base state

```json
{
  "pose": {"x_m": 1.2, "y_m": -0.4, "yaw_rad": 0.52},
  "velocity": {"x_m_s": 0.2, "y_m_s": 0.0, "yaw_rad_s": 0.1},
  "posture": "walk"
}
```

The pose is backend odometry in the coordinate frame configured for that base. The HAL does not attach a universal `map` frame to it: some bases report local odometry, while a localization-capable backend may report a map-relative pose. The binding and backend must agree on the frame.

## Gimbal state

```json
{
  "pan_rad": 0.2,
  "tilt_rad": -0.1,
  "pan_rate_rad_s": 0.0,
  "tilt_rate_rad_s": 0.0
}
```

## Manipulator state

```json
{
  "joint_angles_rad": [0.0, -0.4, 0.6, 0.0, -0.3, 0.7],
  "joint_velocities_rad_s": [0.0, 0.02, 0.01, 0.0, 0.0, 0.0],
  "joint_valid": [true, true, true, true, true, true],
  "position_m": [0.45, 0.0, 0.25],
  "orientation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
  "pose_valid": true
}
```

`joint_valid` and `pose_valid` distinguish an unavailable measurement from a physical zero. The end-effector pose uses the manipulator base frame defined by the robot description.

## Gripper state

```json
{
  "state": "holding",
  "position_pct": 34.0,
  "force_n": 6.1
}
```

Position and force are optional. A gripper can report jaw position, force, or a holding state, but it cannot identify the held object. Object association is an EOS perception result and is deliberately absent from the hardware contract.

## Speaker state

```json
{
  "state": "playing",
  "asset_id": "asset://alerts/ready.wav",
  "queued_duration_ms": 240,
  "played_duration_ms": 760,
  "underruns": 0,
  "source_timestamp_ns": 1789983000000000000
}
```

This is output-device status, not a world observation. All members except `state` and `underruns` are optional. `asset_id` is the EOS-owned identity of the complete asset accepted by the backend. State is `idle`, `buffering`, `playing`, `draining`, or `fault`. Northbound body discovery projects only common speaker state and health, not this device-specific telemetry.

An absent telemetry read means that no valid sample is available; EOS must not fabricate a zero-valued state. The aggregate examples above group the native per-device return types for documentation and do not imply one serialized body message.

`eos/hardware/backend.h` is authoritative. No `share/contracts/southbound` schema tree exists, and HAL values are not validated as Agent contracts.

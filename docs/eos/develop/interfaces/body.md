# Body discovery and state

[中文](../../../../docs_cn/eos/develop/interfaces/body_cn.md)

[Interface overview](README.md) · [World and detection](world.md) · [Primitives](actions.md) · [Skills](skills.md) · [Workflows](workflow.md)

Body is the EOS-owned inventory of the robot. It tells a robotic agent what is attached, whether it is usable, and which EOS primitives are bound to it. It does not expose driver registration, backend details, raw telemetry, images, audio, or perception results.

EOS builds this inventory during initialization from compiled primitive schemas, the selected deployment profile, admitted backends, and dependency health. A northbound client is only a user of the resulting surface: it cannot register, replace, or remove a device or primitive.

## Discover

`GatewayService.Discover` takes an empty `DiscoverRequest`. This excerpt includes the robot-wide bindings and one device–primitive binding:

```json
{
  "robot": {
    "id": "demo-0",
    "name": "demo-0",
    "model": "eos-demo",
    "description": "Deterministic seven-device loopback robot",
    "state": "idle",
    "health": "ready",
    "estop": false,
    "powerPct": 84.5,
    "primitives": [
      {
        "name": "robot.stop",
        "version": "1.0.0",
        "description": "Bring every active robot part to a controlled stop.",
        "kind": "direct",
        "health": "ready",
        "references": [],
        "limits": {},
        "dependencies": [],
        "accepts": [],
        "parameters": {"type":"object","properties":{},"additionalProperties":false}
      },
      {
        "name": "safety.estop",
        "version": "1.0.0",
        "description": "Engage the robot-wide emergency stop.",
        "kind": "direct",
        "health": "ready",
        "references": [],
        "limits": {},
        "dependencies": [],
        "accepts": [],
        "parameters": {"type":"object","properties":{},"additionalProperties":false}
      }
    ]
  },
  "devices": [
    {
      "id": "base-0",
      "class": "base",
      "part": "default",
      "description": "Planar mobile base",
      "state_uri": "robots/demo-0/devices/base-0/state",
      "state": "idle",
      "health": "ready",
      "primitives": [
        {
          "name": "base.navigate",
          "version": "1.0.0",
          "description": "Move the base to a relative or map-frame planar goal.",
          "kind": "direct",
          "health": "ready",
          "references": ["relative", "absolute"],
          "absoluteFrame": "map",
          "limits": {
            "x_m": {"minimum": -10.0, "maximum": 10.0},
            "y_m": {"minimum": -10.0, "maximum": 10.0},
            "yaw_rad": {"minimum": -6.283, "maximum": 6.283}
          },
          "dependencies": [],
          "accepts": [],
          "parameters": {
            "type": "object",
            "properties": {
              "reference": {"type":"string","enum":["relative","absolute"]},
              "frame_id": {"type":"string","minLength":1,"enum":["map"]},
              "x_m": {"type":"number"},
              "y_m": {"type":"number"},
              "yaw_rad": {"type":"number"}
            },
            "required": ["reference","x_m","y_m","yaw_rad"],
            "additionalProperties": false
          }
        }
      ]
    }
  ]
}
```

The body response is the only northbound discovery surface for devices and primitive bindings. Each device's `state_uri` links to its readable compact health/state resource. Each `primitives` array contains only calls that EOS has admitted at that robot or device scope. A target-reference mode is usable only when it appears in `references`; for example, a deployment may expose relative base navigation while withholding absolute navigation until localization is healthy.

The robot descriptor contains the robot-wide primitive bindings that do not belong to one device. These calls use an empty `deviceId`; a client must not infer them from a device binding. The common device fields are `id`, `class`, `part`, `description`, `state_uri`, `state`, `health`, and `primitives`. Each primitive binding contains its exact `name`, `version`, deployment-specific `description`, management `kind`, health, supported reference modes, optional absolute frame, effective argument schema in `parameters`, enforced limits, accepted media types, and advanced dependencies when applicable. A binding is the deployment-specific admission of a primitive at its stated scope; it is not merely a reference to a global catalog. `state` is a short operational enum such as `disabled`, `idle`, `active`, or `fault`; when available telemetry cannot prove either activity or quiescence it is `unknown`; `health` is `ready`, `degraded`, or `unavailable`.

## Device state resources

Every admitted device class shown by `Discover` is readable at `robots/{robot_name}/devices/{device_id}/state` and appears in `ListResources`. The URI uses the configured robot name; EOS maps it to the internal robot ID and rejects devices owned by another robot. These compact body reads are projected directly from hardware and use the existing polling watch path; they do not start a model producer. `robots/{robot_name}/arm/state` remains available as the compatibility alias for detailed manipulator joint telemetry.

EOS also publishes normalized status resources for admitted body devices:

- `robots/{robot_name}/base/status` contains health, freshness/validity flags, valid map-frame pose, optional measured forward speed, normalized localization/motion/charge/dock state, power/odometry when available, and bounded raw numeric diagnostics for vendor-only fields. It does not project the legacy planar velocity or posture defaults as measurements.
- `robots/{robot_name}/arm/status` contains health, configured joint names, measured joint positions and velocities with separate validity arrays, an end-effector pose when valid, and controller/per-joint diagnostics when a correlated status sample exists. Raw current/temperature/voltage registers remain explicitly raw and are not relabeled as SI units.
- `robots/{robot_name}/gripper/status` contains health, jaw state, and optional normalized position and calibrated force.

Unavailable optional measurements are omitted rather than reported as zero. All three URIs appear in `ListResources` and use the ordinary `ReadResource` and `WatchResource` paths, including their HTTP read and SSE projections.

Gimbal device state additionally includes `pan_rad`, `tilt_rad`, `pan_rate_rad_s`, and `tilt_rate_rad_s` when its proprioception is available. Rates are measured by the backend; deployments that omit joint velocity data may report zero and should not treat that value as proof of motion quality.

## Complete device descriptor examples

These examples focus on inventory and operating envelopes and omit the `parameters` schema already illustrated above. Live discovery always includes it on every primitive binding.

### Base

```json
{
  "id": "base-0",
  "class": "base",
  "part": "default",
  "description": "Planar mobile base",
  "state": "idle",
  "health": "ready",
  "primitives": [
    {
      "name": "base.velocity",
      "version": "1.0.0",
      "description": "Drive using leased body-frame velocities.",
      "kind": "direct",
      "health": "ready",
      "references": [],
      "limits": {
        "linear_x_m_s": {"minimum": -1.0, "maximum": 1.0},
        "linear_y_m_s": {"minimum": -1.0, "maximum": 1.0},
        "angular_z_rad_s": {"minimum": -1.0, "maximum": 1.0},
        "valid_for_ms": {"minimum": 1, "maximum": 1000}
      },
      "dependencies": [],
      "accepts": []
    },
    {
      "name": "base.navigate",
      "version": "1.0.0",
      "description": "Move to a relative or map-frame planar goal.",
      "kind": "direct",
      "health": "ready",
      "references": ["relative", "absolute"],
      "absoluteFrame": "map",
      "limits": {
        "x_m": {"minimum": -10.0, "maximum": 10.0},
        "y_m": {"minimum": -10.0, "maximum": 10.0},
        "yaw_rad": {"minimum": -6.283, "maximum": 6.283}
      },
      "dependencies": [],
      "accepts": []
    }
  ]
}
```

### Manipulator

```json
{
  "id": "arm-0",
  "class": "manipulator",
  "part": "default",
  "description": "Six-axis manipulator",
  "state": "idle",
  "health": "ready",
  "primitives": [
    {
      "name": "arm.movej",
      "version": "1.0.0",
      "description": "Move every joint to an absolute target or by a relative delta.",
      "kind": "direct",
      "health": "ready",
      "references": ["relative", "absolute"],
      "limits": {
        "joint_positions_rad": {"minimum": -3.142, "maximum": 3.142},
        "speed_rad_s": {"minimum": 0.0, "minimumExclusive": true}
      },
      "dependencies": [],
      "accepts": []
    },
    {
      "name": "arm.movel",
      "version": "1.0.0",
      "description": "Move the tool to an absolute pose or by a relative transform.",
      "kind": "direct",
      "health": "ready",
      "references": ["relative", "absolute"],
      "absoluteFrame": "base_link",
      "limits": {
        "speed_m_s": {"minimum": 0.0, "minimumExclusive": true}
      },
      "dependencies": [],
      "accepts": []
    }
  ]
}
```

### Gimbal

```json
{
  "id": "gimbal-0",
  "class": "gimbal",
  "part": "default",
  "description": "Pan/tilt vision mount",
  "state": "idle",
  "health": "ready",
  "primitives": [
    {
      "name": "gimbal.motion",
      "version": "1.0.0",
      "description": "Aim the gimbal using relative or absolute pan and tilt targets.",
      "kind": "direct",
      "health": "ready",
      "references": ["relative", "absolute"],
      "absoluteFrame": "base_link",
      "limits": {
        "pan_rad": {"minimum": -3.142, "maximum": 3.142},
        "tilt_rad": {"minimum": -1.571, "maximum": 1.571},
        "speed_rad_s": {"minimum": 0.0, "minimumExclusive": true}
      },
      "dependencies": [],
      "accepts": []
    }
  ]
}
```

### Gripper

```json
{
  "id": "gripper-0",
  "class": "gripper",
  "part": "default",
  "description": "Parallel jaw gripper",
  "state": "idle",
  "health": "ready",
  "primitives": [
    {
      "name": "gripper.motion",
      "version": "1.0.0",
      "description": "Open or close the gripper with optional force.",
      "kind": "direct",
      "health": "ready",
      "references": [],
      "limits": {
        "force_n": {"minimum": 0.0, "minimumExclusive": true, "maximum": 10.0}
      },
      "dependencies": [],
      "accepts": []
    }
  ]
}
```

## Sensor and speaker devices

Camera, microphone, and speaker entries expose no device-specific data beyond health/state; a speaker may still carry the common primitive bindings. Camera frames are returned by [world vision snapshots](world.md#vision-snapshot), and microphone audio never crosses northbound.

```json
{
  "id": "cam-0",
  "class": "camera",
  "part": "front",
  "description": "Front RGB camera",
  "state": "capturing",
  "health": "ready",
  "primitives": []
}
```

```json
{
  "id": "mic-0",
  "class": "microphone",
  "part": "head",
  "description": "Head microphone array",
  "state": "listening",
  "health": "ready",
  "primitives": []
}
```

The speaker advertises the direct file-play operation and, when configured, the advanced TTS chain. The advanced binding reports its dependency health so an agent can distinguish a failed TTS service from a failed output device.

```json
{
  "id": "speaker-0",
  "class": "speaker",
  "part": "head",
  "description": "Head speaker",
  "state": "idle",
  "health": "degraded",
  "primitives": [
    {
      "name": "speaker.play",
      "version": "1.0.0",
      "description": "Play one EOS-owned WAV asset.",
      "kind": "direct",
      "health": "ready",
      "references": [],
      "limits": {},
      "dependencies": [],
      "accepts": ["audio/wav"]
    },
    {
      "name": "speaker.speak",
      "version": "1.0.0",
      "description": "Synthesize text and play it through this speaker.",
      "kind": "advanced",
      "health": "unavailable",
      "references": [],
      "limits": {},
      "dependencies": [
        {"name": "tts", "health": "unavailable"},
        {"name": "speaker", "health": "ready"}
      ],
      "accepts": []
    }
  ]
}
```

No backend name, transport endpoint, codec internals, frame rate, sample rate, buffer depth, or raw payload belongs in body discovery. Those details remain inside EOS unless they constrain an agent-callable primitive.

# Primitive calls

[中文](../../../../docs_cn/eos/develop/interfaces/actions_cn.md)

[Interface overview](README.md) · [Body discovery](body.md) · [World and detection](world.md) · [Skills](skills.md) · [Workflows](workflow.md)

Northbound clients call primitives owned, registered, validated, and admitted by EOS. There is no northbound primitive registry or mutation API. An agent learns the usable primitive bindings, effective argument schemas, modes, and limits from [body discovery](body.md), then sends one intent with `GatewayService.CallPrimitive`. Every call carries a non-empty, globally unique `commandId` in the invocation envelope and returns one terminal response.

EOS auto-registers the built-in primitive catalog during runtime initialization, binds catalog entries to devices from the selected profile, and publishes the policy-admitted bindings with their current health. A temporarily unhealthy binding remains discoverable as degraded or unavailable so clients can inspect its dependency state. A client cannot add a primitive or device by submitting metadata.

Each discovered binding's `parameters` field is an object-shaped JSON Schema for the caller-owned `arguments` object. EOS removes its injected `type` and `command_id` fields, narrows `reference` and `frame_id` to the selected binding, fixes device structure such as arm joint count, and omits optional numeric controls unsupported by that profile. Canonical structural constraints and conditionals remain intact. The separate `limits` map remains authoritative for deployment/runtime numeric envelopes, especially relative motion; clients should present it as an additional hint rather than rewriting those values into static schema bounds.

## Primitive kinds

EOS has two management kinds:

- `direct`: one validated device or safety operation, including all motion.
- `advanced`: an EOS-owned composition with explicit dependencies, such as TTS followed by speaker playback.

Both kinds use the same call and terminal response envelopes. `kind` is discovery metadata and is not repeated by the caller. Every call returns a non-null `result` block; EOS never places raw device telemetry, provider payloads, images, or audio in that block.

## Command lifecycle

`commandId` is the idempotency and cancellation key for one primitive command. EOS durably reserves it before the first backend call. The first request may actuate once; an identical concurrent request joins that command and an identical later request receives the stored terminal response. Reusing the ID with a different primitive, target, version, or arguments fails with `ALREADY_EXISTS` and never reaches a backend.

EOS and its backends never retry an admitted primitive after a release, timeout, lost response, cancellation, or ambiguous transport failure. A crash after durable reservation but before a terminal record is recovered as `outcome_unknown`, never re-released. Deployment-owned ROS 2 command nodes must apply the same persistent command-ID rule: duplicate delivery returns the recorded result and does not actuate again.

`CallPrimitive` waits for a terminal status: `succeeded`, `rejected`, `failed`, `cancelled`, `timed_out`, or `outcome_unknown`. Admission and physical success are separate fields; an admitted command can still fail or be cancelled. For `stream: true`, the requested operation ends at successful target publication, so `succeeded` does not assert physical arrival. A completed stream record is replayed for duplicate IDs; callers stop motion with `motion.stop` or supersede it with a newer stream target rather than cancelling that completed publication.

The cancellable primitive set is explicit: `base.velocity`, `base.navigate`, `base.posture`, `gimbal.motion`, `arm.init`, `arm.movej`, `arm.movel`, `arm.pick`, and `gripper.motion`. `CancelPrimitive` uses `GatewayService.CancelPrimitive` with the command ID. Cancellation is idempotent and command-targeted: it never stops a later command on the same device. A terminal command returns its stored response; an unknown ID is not found, and a non-motion command fails the precondition. Client disconnect and deadline expiry do not implicitly cancel a command.

## Motion references

Every pose- or position-targeting motion uses a `reference` value:

- `relative` composes a delta with the current fresh admitted state.
- `absolute` targets a value in `frame_id` (or the device's configured absolute frame when the primitive has no frame field).

EOS advertises allowed values per binding. A deployment may disable either mode; EOS rejects an unadvertised mode with `rejected.unsupported_reference` before calling a backend. Relative and absolute behavior therefore share one primitive name and one safety/limit path.

Each applicable `[[profile.*.device.primitive]]` entry declares `references = ["relative", "absolute"]` or a policy-restricted subset. Absolute Cartesian or navigation bindings also declare `absolute_frame = "base_link"` or `"map"`. Startup rejects an empty/unknown mode or an absolute binding without a qualified frame. These keys live in the deployment profiles rather than being inferred from backend type.

Velocity remains a leased rate command in its declared body frame, so it does not pretend to be absolute motion.

The four reference-aware primitives use the following exact compositions:

- `base.navigate`: a relative planar SE(2) transform in the robot frame; absolute coordinates in the advertised map frame.
- `arm.movej`: per-joint deltas added to fresh joint positions; absolute joint positions in configured joint order.
- `arm.movel`: a relative SE(3) tool transform composed with the fresh tool pose; an absolute pose in the advertised base frame.
- `gimbal.motion`: pan/tilt deltas added to fresh angles; absolute angles about the device zero.

Each reference-aware target primitive accepts optional `stream: true`. Normal requests wait for the backend's correlated terminal result. A stream request returns after the target is published so a controller can consume later target updates without making the caller wait at every intermediate point. Stream mode does not weaken validation, reference resolution, limits, command IDs, device health, stop, or estop. A southbound adapter must explicitly implement streaming for physical continuity; the field is not permission to overlap ordinary blocking moves.

A deployment adapter that supports streamed `base.navigate` must define how it replaces targets, settles cancellation, handles stale localization, and stops on inactivity. EOS still validates each target and preserves `motion.stop` and estop authority.

Velocity, posture, initialization, picking, gripper open/close, stop, audio, and face registration are semantic operations rather than coordinate targets; they do not accept a fabricated `reference` field.

## Arm initialization

```json
{
  "name": "arm.init",
  "version": "1.0.0",
  "deviceId": "arm-0",
  "commandId": "arm-init-1",
  "arguments": {}
}
```

## Base navigation

```json
{
  "name": "base.navigate",
  "version": "1.0.0",
  "deviceId": "base-0",
  "commandId": "nav-9",
  "arguments": {
    "reference": "absolute",
    "frame_id": "map",
    "x_m": 4.2,
    "y_m": -1.1,
    "yaw_rad": 1.57
  }
}
```

With `relative`, `frame_id` is omitted and the values are deltas in the robot frame. Both modes deliberately share this primitive identity and admission path.

## Joint motion

```json
{
  "name": "arm.movej",
  "version": "1.0.0",
  "deviceId": "arm-0",
  "commandId": "movej-17",
  "arguments": {
    "reference": "absolute",
    "joint_positions_rad": [0.0, -0.4, 0.6, 0.0, -0.3, 0.7],
    "speed_rad_s": 0.25
  }
}
```

For `relative`, each value is added to the corresponding current joint. EOS requires a fresh complete joint sample before admitting the call.

## Cartesian motion

```json
{
  "name": "arm.movel",
  "version": "1.0.0",
  "deviceId": "arm-0",
  "commandId": "movel-18",
  "arguments": {
    "reference": "absolute",
    "frame_id": "base_link",
    "position_m": [0.45, 0.0, 0.25],
    "orientation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
    "speed_m_s": 0.25
  }
}
```

For `relative`, the pose is a transform from the current tool pose and `frame_id` is omitted.

## Pick a world object

`arm.pick` selects one non-human object currently named by a [`detection.snapshot`](world.md#semantic-detection). The ID must use the `obj:<track-or-domain-id>` namespace; human IDs are never valid pick targets.

```json
{
  "name": "arm.pick",
  "version": "1.0.0",
  "deviceId": "arm-0",
  "commandId": "pick-19",
  "arguments": {
    "object_id": "obj:part-7"
  }
}
```

EOS validates the typed identifier and sends it directly to the admitted arm backend. For the ROS 2 backend, a deployment-owned pick node receives the ID and owns current-world lookup, freshness, pickability, planning, grasp selection, and execution. The node returns a command-correlated terminal result and accepts command-correlated cancellation.

## Gimbal motion

```json
{
  "name": "gimbal.motion",
  "version": "1.0.0",
  "deviceId": "gimbal-0",
  "commandId": "gimbal-6",
  "arguments": {
    "reference": "relative",
    "pan_rad": 0.3,
    "tilt_rad": -0.15,
    "speed_rad_s": 0.5
  }
}
```

## Gripper motion

```json
{
  "name": "gripper.motion",
  "version": "1.0.0",
  "deviceId": "gripper-0",
  "commandId": "grip-4",
  "arguments": {
    "action": "close",
    "force_n": 6.0
  }
}
```

The motion name is consistent with other actuators. `action` is `open` or `close`; a future position-capable gripper may additionally admit a position target without introducing another command-style primitive.

## Other direct primitives

These operations are not pose targets and therefore do not take `reference`.

### Base posture

```json
{
  "name": "base.posture",
  "version": "1.0.0",
  "deviceId": "base-0",
  "commandId": "posture-4",
  "arguments": {
    "posture": "stand"
  }
}
```

### Leased base velocity

```json
{
  "name": "base.velocity",
  "version": "1.0.0",
  "deviceId": "base-0",
  "commandId": "velocity-42",
  "arguments": {
    "linear_x_m_s": 0.2,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.1,
    "valid_for_ms": 500
  }
}
```

### Part stop

`motion.stop` performs a controlled stop of the motion owned by exactly one device. `deviceId` is required and must name a device that advertises the binding. The call does not stop other parts. It succeeds only after EOS reaches the device's terminal stopped boundary; calling it for an already stopped device is successful and has no additional effect.

```json
{
  "name": "motion.stop",
  "version": "1.0.0",
  "deviceId": "arm-0",
  "commandId": "stop-3",
  "arguments": {}
}
```

### Whole-robot stop

`robot.stop` requests a controlled stop of every active robot part. It is a robot-level binding advertised on the robot descriptor, so `deviceId` is empty. EOS fans the stop out to every active actuator, waits for all of their terminal stop boundaries, and reports failure if any part cannot be confirmed stopped. It is idempotent and does not engage the emergency-stop circuit.

```json
{
  "name": "robot.stop",
  "version": "1.0.0",
  "deviceId": "",
  "commandId": "robot-stop-2",
  "arguments": {}
}
```

### Emergency stop

`safety.estop` is the distinct robot-wide emergency operation. It is also a robot-level binding and therefore uses an empty `deviceId`. It engages the configured emergency-stop path and succeeds only after EOS receives its terminal confirmation. This interface does not clear or reset an emergency stop.

```json
{
  "name": "safety.estop",
  "version": "1.0.0",
  "deviceId": "",
  "commandId": "estop-1",
  "arguments": {}
}
```

## Speaker play

`speaker.play` plays one EOS-readable audio asset. It does not expose chunks, stream IDs, buffering, or raw audio transport northbound.

```json
{
  "name": "speaker.play",
  "version": "1.0.0",
  "deviceId": "speaker-0",
  "commandId": "play-8",
  "arguments": {
    "uri": "asset://alerts/ready.wav"
  }
}
```

EOS resolves the URI, enforces allowed schemes and size/type limits, decodes as needed, and owns playback through completion or cancellation.

## Speaker speak (advanced)

`speaker.speak` is an advanced primitive: EOS validates text and voice options, invokes the admitted TTS provider, validates its output, and plays it through the selected speaker. The hardware profile binds this primitive to one exact enabled `audio.synthesis` model; EOS refuses launch rather than substituting a different TTS model when that dependency is absent or disabled.

```json
{
  "name": "speaker.speak",
  "version": "1.0.0",
  "deviceId": "speaker-0",
  "commandId": "speak-3",
  "arguments": {
    "text": "The loading bay is clear.",
    "language": "en",
    "voice": "default"
  }
}
```

The binding is callable only while the whole TTS → audio validation → speaker chain is healthy. Its acknowledgement may identify the failed stage without exposing provider credentials or internal transport.

## Face registration (advanced)

`face.register` enrolls one face from the latest valid JPEG published by the selected camera. The camera binding names one exact enabled `vision.face` model; EOS does not substitute the background recognition model. Registration does not trigger a capture, use the `CaptureFrames` cache, or collect a burst.

```json
{
  "name": "face.register",
  "version": "1.0.0",
  "deviceId": "cam-0",
  "commandId": "face-register-1",
  "arguments": {
    "name": "Person A"
  }
}
```

EOS trims surrounding whitespace and rejects an empty name or the reserved name `unknown` case-insensitively. A successful acknowledgement contains the provider-confirmed identity and source camera:

```json
{
  "type": "primitive.ack",
  "primitive": "face.register",
  "primitive_version": "1.0.0",
  "accepted": true,
  "reason": null,
  "reason_code": null,
  "command_id": "face-register-1",
  "sequence": 31,
  "result": {"status": "succeeded", "name": "Person A", "deviceId": "cam-0"}
}
```

The provider owns persistence. Later recognition may expose the identity as `detection.snapshot.detections[].attributes.name`; registration does not rewrite an already published snapshot.

The public primitive catalog exposes one current contract per name at `1.0.0`. For `arm.movej` and `arm.movel`, that contract includes required reference data; for `speaker.play`, it uses an asset URI. `gripper.motion` includes optional opening and speed fields. EOS does not accept earlier payload forms under these identities.

## Terminal response

`CallPrimitive` returns exactly one `primitive.ack`. The `accepted` field says whether EOS admitted the request. The required, non-null `result` block reports the terminal outcome:

- `succeeded`: the primitive reached its defined terminal boundary;
- `failed`: EOS admitted and released the operation, but it subsequently failed;
- `rejected`: EOS did not release the operation.

Each primitive defines its terminal boundary. Motion completes at its admitted goal, leased velocity completes when the lease is established, playback and speech complete when output ends, and stop operations complete after stop is confirmed. `result` contains only the semantic outcome and optional primitive-defined semantic values. It never contains raw sensor samples, backend responses, audio, images, or transport details.

A schema-valid `CallPrimitive` request always receives this envelope, including policy, state, target, and argument rejections. A request that cannot be decoded as the transport message is a transport error and has no primitive identity or result block.

```json
{
  "type": "primitive.ack",
  "primitive": "base.navigate",
  "primitive_version": "1.0.0",
  "accepted": true,
  "reason": null,
  "reason_code": null,
  "command_id": "nav-9",
  "sequence": 31,
  "result": {
    "status": "succeeded"
  }
}
```

A rejected acknowledgement retains the same complete shape:

```json
{
  "type": "primitive.ack",
  "primitive": "gripper.motion",
  "primitive_version": "1.0.0",
  "accepted": false,
  "reason": "gripper is already executing a motion",
  "reason_code": "rejected.busy",
  "command_id": "grip-4",
  "sequence": 32,
  "result": {
    "status": "rejected"
  }
}
```

An admitted operation that later fails retains `accepted: true` and keeps the top-level `reason` and `reason_code` null. Its `result.status` is `failed`, and any stable machine detail or concise human-readable explanation belongs inside `result`:

```json
{
  "type": "primitive.ack",
  "primitive": "base.navigate",
  "primitive_version": "1.0.0",
  "accepted": true,
  "reason": null,
  "reason_code": null,
  "command_id": "nav-10",
  "sequence": 33,
  "result": {
    "status": "failed",
    "reason_code": "failed.execution",
    "reason": "navigation backend could not reach the admitted goal"
  }
}
```

Rejections use stable codes including `rejected.invalid_arguments`, `rejected.invalid_target`, `rejected.invalid_state`, `rejected.unsupported_reference`, `rejected.unavailable_dependency`, `rejected.unauthorized`, `rejected.busy`, `rejected.command_conflict`, and `rejected.internal`.

## Command identity and retries

`command_id` is required and provides idempotency, not just log correlation. It must be globally unique and must never be reused for a different command, including after EOS or a downstream controller restarts.

EOS fingerprints the primitive name, version, device ID, and canonical arguments for the first request carrying a command ID. A concurrent duplicate with the same fingerprint joins the original call; a later duplicate returns the original terminal response, including its sequence, without releasing the operation again. Reusing the ID with a different fingerprint is rejected with `rejected.command_conflict`.

The durable deduplication record survives EOS restart. A client may retry the same request and receive the recorded terminal response; a reservation without a terminal journal entry recovers as `outcome_unknown` and is never released again. After any restart, clients must reconcile robot state before issuing dependent work and must mint a new ID for every new command.

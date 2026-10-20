# Primitive Payload Contracts

[Interface overview](README_cn.md) links the contract surfaces and runtime owners.

`CallPrimitive` releases one validated device intent. This is the client-facing reference for the canonical JSON Schemas under `share/ext/eos/actions/`; [northbound.md](grpc_cn.md) defines transport and the complete runtime flow.

## Call envelope

Native gRPC calls `GatewayService.CallPrimitive(CallPrimitiveRequest)`. The external adapter maps `POST /v1/eos/primitives:call` with protobuf JSON names.

```json
{
  "name": "base.velocity",
  "version": "1.0.0",
  "deviceId": "base-0",
  "commandId": "move-42",
  "arguments": {
    "linear_x_m_s": 0.3,
    "linear_y_m_s": 0.0,
    "angular_z_rad_s": 0.1,
    "valid_for_ms": 250
  }
}
```

精确的 `{name, version}` 选择 primitive contract；当前所有 primitive 均使用 `1.0.0`。缺失或未知版本无效。`deviceId` 选择生成的 `<class>-<index>` 设备，`arguments` 使用 schema 的 snake-case 字段名。

## Discovery descriptions

每个 primitive kind 都有非空 catalog 描述。每个 `[[device.primitive]]` 声明也有可为空的 `description`，说明该操作在具体设备上的意义。`Discover.devices[].primitives` 返回已接纳 binding 的精确 name/version、`direct` 或 `advanced` kind、允许的 references、limits、health、dependency health 与有效 `parameters` JSON Schema。该 schema 不含 EOS 注入的 `type`/`command_id`，并按所选设备收窄 reference、frame、joint array 结构及可选数值字段。Northbound 没有 primitive registry 或 mutation API。

> **Note:** Use the kind description to understand the portable contract. Use the device description for deployment-specific guidance such as reachable workspace, tooling, or intended purpose.

Unused primitives need no device declaration or device description. Privileged `motion.stop` and `safety.estop` are implicit and are not listed as device declarations.

## Targeting, units, and validation

Device primitives require a non-empty target of the matching class. `motion.stop` targets a base, arm, or gimbal. Process-wide `safety.estop` requires an empty `deviceId`.

Command fields use explicit standard units so an agent can write a request without knowing a vendor-specific normalization curve. Linear values use metres, angular values use radians, force uses newtons, and durations use milliseconds. Field names carry the unit suffix (`_m`, `_m_s`, `_rad`, `_rad_s`, `_n`, or `_ms`). Omitted optional speed and force fields select the backend default. A supplied optional value must have a discovered device interval; absence of that interval means the device supports only the omitted/default form.

共享 action schema 只负责类型、enum 与数组形状等可移植约束。数值范围在 EOS admission 时解析，并直接出现在所选设备的 primitive binding 中；没有可读的 backend-capabilities JSON resource。Discovery 发布范围，dispatch 强制执行，backend 仍是 URDF、workspace、collision 与 controller safety 的最终权威。每个 schema 保留 EOS 注入的 nullable `command_id`；northbound 客户端使用 envelope 顶层 `commandId` 并省略 `type`，EOS 根据 envelope 的 `{name, version}` 在校验前补入这两个内部字段。

## Payloads

| Primitive | Target | Arguments |
| --- | --- | --- |
| `base.velocity` | `base-*` | Required `linear_x_m_s`, `linear_y_m_s`, `angular_z_rad_s`, and integer `valid_for_ms` within the selected device's discovered limits |
| `base.navigate` | `base-*` | 必填 `reference`、`x_m`、`y_m`、`yaw_rad`；absolute 时还需已发现的 `frame_id`；可选 `stream` |
| `base.posture` | `base-*` | Required `posture`: `stand`, `walk`, or `lie` |
| `gimbal.motion` | `gimbal-*` | 必填 `reference`、`pan_rad`、`tilt_rad`，可选 `speed_rad_s` 与 `stream` |
| `arm.init` | `arm-*` | No action-specific fields; moves to the selected device's configured initial joint pose |
| `arm.movej` | `arm-*` | 必填 `reference` 与完整 `joint_positions_rad`；可选 `speed_rad_s` 与 `stream` |
| `arm.movel` | `arm-*` | 必填 `reference`、`position_m` 与 `orientation_quat_xyzw`；absolute 时还需 `frame_id`；可选 `speed_m_s` 与 `stream` |
| `gripper.motion` | `gripper-*` | 必填 `action`（`open` 或 `close`）；可选 `force_n` |
| `speaker.play` | `speaker-*` | 必填 EOS 可读的音频 asset `uri` |
| `speaker.speak` | `speaker-*` | 高级 primitive；必填 `text`，可选 `language` 与 `voice` |
| `motion.stop` | base, arm, or gimbal | No action-specific fields |
| `robot.stop` | empty device ID | 无；并行停止所有 motion-capable part，并聚合终态确认 |
| `safety.estop` | empty device ID | No action-specific fields |

### Motion examples

四个 reference-aware target primitive 都接受可选的 `stream: true`。默认模式等待关联的终态结果；stream 模式在发布目标后立即返回，供兼容的 adapter 连续合并新目标。此时 `succeeded` 表示目标已成功发布，不表示已物理到达。已完成的 stream publication 会按 command ID 重放；调用方应使用 `motion.stop` 停止运动，或使用更新的 stream target 覆盖目标，而不是取消已经完成的 publication。Stream 不会绕过 validation、reference resolution、limits、health、stop 或 estop。

The base validity lease is fail-closed: EOS clamps the base when it expires.

```json
{"linear_x_m_s": 0.3, "linear_y_m_s": 0.0, "angular_z_rad_s": -0.1, "valid_for_ms": 250}
```

Relative navigation uses the base frame at submission time and completes only after the backend returns its final navigation result.

```json
{"reference":"relative","x_m":1.0,"y_m":-0.5,"yaw_rad":0.25}
```

对 gimbal，零弧度是各轴中心。选择目标前从 `Discover` 的设备 primitive binding 读取允许的 reference 与非对称物理范围。

```json
{"reference":"absolute","pan_rad":0.7,"tilt_rad":0.0,"speed_rad_s":0.4}
```

`arm.movej` is a complete joint target, not a single-joint or masked command. Its array length must equal the selected device's configured `joint_names` count; the shared v1 schema permits one through six entries.

```json
{"reference":"absolute","joint_positions_rad":[0.0,-0.4,0.6,0.0,-0.3,0.7],"speed_rad_s":0.25}
```

`arm.movel` keeps its task-space position in meters. The quaternion must be finite, in x/y/z/w order, and unit length.

```json
{"reference":"absolute","frame_id":"base_link","position_m":[0.45,0.0,0.25],"orientation_quat_xyzw":[0.0,0.0,0.0,1.0],"speed_m_s":0.25}
```

### 已配置的机械臂初始姿态

`arm.init` 是无参数 primitive。部署绑定的 `initial_joint_positions_rad` 必须按已配置的 joint 顺序给出经控制器验证的目标。只有该设备声明了 `arm.init`，才能显式调用：

```bash
curl --fail-with-body --silent --show-error \
  -H 'Content-Type: application/json' \
  --data '{"name":"arm.init","version":"1.0.0","deviceId":"arm-0","arguments":{}}' \
  http://127.0.0.1:16005/v1/eos/primitives:call
```

`arm.init` does not run at startup and does not change `arm.movej` or `arm.movel`. It releases the configured pose through the same joint-motion backend path only when explicitly called. For ROS 2, `accepted: true` confirms local command publication; verify physical arrival from arm feedback.

### Discrete and safety examples

```json
{"action": "close", "force_n": 6.0, "command_id": "grasp-42"}
```

```json
{"uri":"asset://alerts/ready.wav","command_id":"play-42"}
```

```json
{"command_id": "stop-42"}
```

`motion.stop` keeps its targeted device in a safe stopped state. `safety.estop` halts the complete runtime and remains latched until EOS restarts.

## Acknowledgements

Every completed call returns the canonical `primitive.ack` object from `primitives/actions/ack.v1.json`.

```json
{
  "type": "primitive.ack",
  "primitive": "base.velocity",
  "primitive_version": "1.0.0",
  "accepted": true,
  "reason": null,
  "reason_code": null,
  "command_id": "move-42",
  "sequence": 17,
  "result": null
}
```

预期的 runtime 或 backend 准入拒绝仍是成功的 RPC，并返回 `accepted: false`。必填的顶层 `reason` 是拒绝层提供给操作员和 Agent 的可读错误消息；顶层 `reason_code` 是稳定的机器可读拒绝码。命令一旦准入，这两个顶层字段保持 null，终态失败的原因与可选稳定代码必须放在 `result` 内。`command_id` 是全局唯一、跨重启不得复用于新命令的幂等键；durable journal 将未完成 reservation 恢复为 `outcome_unknown`，不会重新执行。

## Migration from the earlier v1 payloads

This cleanup rewrites v1 in place and intentionally provides no compatibility aliases. Update clients, skills, and site-local binding files together:

- replace `arm.joint_position` with complete-array `arm.movej`;
- replace `arm.cartesian` with `arm.movel`;
- replace legacy or normalized command fields with the explicit unit-bearing names above;
- remove single-joint, partial-mask, joint-velocity, and low-level Cartesian velocity/wrench calls;
- add `description` to every used `[[device.primitive]]` declaration (an empty string is valid).
- add the required per-device `limits` to bounded primitive declarations.

## Related

- [Northbound interface](grpc_cn.md)
- [Southbound driver interface](southbound/driver_cn.md)
- [Contract loading and validation](contracts_cn.md)
- [Primitive dispatch and safety behavior](../../design/runtime/dispatch_cn.md)

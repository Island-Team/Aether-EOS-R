# Primitive 调用

[English](../../../../docs/eos/develop/interfaces/actions.md) · [接口总览](README_cn.md) · [Body](body_cn.md) · [World](world_cn.md) · [参数示例](actions-payloads_cn.md)

EOS 拥有 primitive 的注册、设备绑定、参数验证与最终释放。客户端从 [Discover](body_cn.md) 读取有效绑定、`parameters` schema、reference 模式和 `limits`，然后调用 `GatewayService.CallPrimitive`。Northbound 没有 primitive 注册或修改接口。每次调用提供非空且全局唯一的 `commandId`，并得到一个终态 `primitive.ack`。

```json
{"name":"base.velocity","version":"1.0.0","deviceId":"base-0","commandId":"move-42","arguments":{"linear_x_m_s":0.3,"linear_y_m_s":0.0,"angular_z_rad_s":0.1,"valid_for_ms":250}}
```

`direct` 表示一次设备或安全操作；`advanced` 表示 EOS 拥有的有依赖组合，例如 TTS 后播放扬声器。两者使用相同请求和终态响应。`kind` 仅是发现元数据，调用方不重复填写。`parameters` 描述调用方拥有的 `arguments`；EOS 注入的 `type` 与 `command_id` 不在其中。设备特定数值范围以发现结果中的 `limits` 为准。

## 命令身份与终态

EOS 在首次向后端释放前持久预留 command ID。相同内容的并发重试加入原命令，之后的相同重试重放原终态；使用相同 ID 但更换 primitive、版本、目标或参数会冲突且不执行。释放后出现超时、取消、响应丢失或模糊传输故障时，EOS 不自动重试。重启后未有终态记录的预留恢复为 `outcome_unknown`，也不会重新释放。部署侧 ROS 2 命令节点须保持同样的去重规则。

终态 `result.status` 可以是 `succeeded`、`rejected`、`failed`、`cancelled`、`timed_out` 或 `outcome_unknown`。`accepted` 只说明是否通过准入，不保证物理成功。一个接纳后失败的结果仍保持 `accepted: true`，顶层 `reason` 为空，详细失败原因在 `result` 内：

```json
{"type":"primitive.ack","primitive":"base.navigate","primitive_version":"1.0.0","accepted":true,"reason":null,"reason_code":null,"command_id":"nav-10","sequence":33,"result":{"status":"failed","reason_code":"failed.execution","reason":"navigation backend could not reach the admitted goal"}}
```

准入拒绝仍返回完整 ack，其 `accepted` 为 `false`、`result.status` 为 `rejected`，顶层给出稳定 `reason_code`。无法解码的传输请求才没有 primitive ack。响应不包含原始遥测、图像、音频或 provider payload。

## Motion reference 与取消

`base.navigate`、`arm.movej`、`arm.movel`、`gimbal.motion` 使用 `reference`。`relative` 基于当前新鲜状态组合增量；`absolute` 使用 binding 允许的坐标系或关节零位。绝对导航与笛卡尔机械臂运动需提供已接纳的 `frame_id`。每个 profile 可以只开放其中一个模式；未开放的模式会在后端调用前被拒绝。速度、posture、初始化、拾取、夹爪、停止、音频和 face registration 不接受伪造的 reference。

这四个目标运动可选 `stream: true`。普通调用等待后端终态；stream 模式在成功发布目标后完成，所以 `succeeded` 不代表已经到达。stream 不绕过校验、范围、健康、stop 或 estop。已经完成的 stream publication 应通过新目标覆盖或用 `motion.stop` 停止。

`CancelPrimitive` 按 command ID 取消仍在进行的可取消操作，不会停止同一设备后来的命令。可取消集合包括 `base.velocity`、`base.navigate`、`base.posture`、`gimbal.motion`、`arm.init`、`arm.movej`、`arm.movel`、`arm.pick` 和 `gripper.motion`。客户端断开和 deadline 到期不会隐式取消。

## 操作类别

`base.velocity` 使用有期限的速率命令；`base.navigate` 使用平面目标；`arm.movej` v1 接收完整关节数组；`arm.movel` v1 接收位置和 xyzw 四元数；`arm.init` 使用部署绑定的初始关节姿态；`arm.pick` v1 接收 `obj:*` 对象 ID 并依赖 ROS 2 pick endpoint。云台运动使用 pan/tilt，夹爪使用 open/close。`speaker.play` v1 接收 EOS 可读取的 WAV asset URI；高级 `speaker.speak` 接收文本，`face.register` 接收姓名。`motion.stop` 面向设备，`robot.stop` 和 `safety.estop` 面向空设备 ID 的全机范围。各操作的完整 payload 示例见 [参数示例](actions-payloads_cn.md) 与 [英文详细接口](../../../../docs/eos/develop/interfaces/actions.md)。

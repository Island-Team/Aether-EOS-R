# 硬件原语接口

[English](../../../../../docs/eos/develop/interfaces/southbound/primitives.md)

[接口总览](../README_cn.md) · [硬件本体](body_cn.md) · [世界输入](world_cn.md)

Dispatcher 向驱动交付已验证的操作。以下 JSON 是原生 C++ 值的逻辑示例。可取消运动携带 `command_id`，连续目标还携带单调时钟时间戳与 `stream`。普通运动等待终态；流式运动发布后即可确认，适配器可合并后续目标，但必须保持停止和急停有效。驱动不得重复执行同一个命令 ID。

## 机械臂关节命令

`arm.init` 和 `arm.movej` 都转换为按设备关节顺序排列的完整弧度目标。

```json
{
  "joint_positions_rad": [0.0, -0.4, 0.6, 0.0, -0.3, 0.7],
  "speed_rad_s": 0.25,
  "stream": false,
  "command_id": "movej-17",
  "stamp_monotonic_ns": 8412345000000
}
```

## 机械臂笛卡尔命令

目标包含米制位置和 XYZW 四元数；逆解与轨迹由后端负责。

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

## 机械臂抓取选择

仅传递目标对象 ID。ROS 2 图边节点负责当前世界状态查询、抓取规划和物理完成。

```json
{
  "object_id": "obj:part-7",
  "command_id": "pick-19",
  "stamp_monotonic_ns": 8412345150000
}
```

## 底盘速度命令

北向租约由 EOS 执行，驱动收到速度和发布时间戳。

```json
{
  "linear_x_m_s": 0.2,
  "linear_y_m_s": 0.0,
  "angular_z_rad_s": 0.1,
  "command_id": "velocity-42",
  "stamp_monotonic_ns": 8412345200000
}
```

## 相对导航命令

位移和偏航是机器人相对目标；流式标志决定发布确认语义。

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

## 底盘姿态命令

允许 `stand`、`walk`、`lie`。

```json
{
  "posture": "stand",
  "command_id": "posture-4"
}
```

## 云台命令

Dispatcher 在交付前解析相对目标，驱动收到完整绝对角度。

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

## 夹爪命令

动作还可为 `open`；未给出力时使用后端已准入的默认值。

```json
{
  "action": "close",
  "force_n": 6.0,
  "command_id": "grip-4",
  "stamp_monotonic_ns": 8412345500000
}
```

## 扬声器命令

`data_b64` 仅表示 C++ 字节向量。EOS 先完成 URI、TTS、大小和 WAV 校验；后端整体接受或拒绝，HAL 不提供分块流协议。

```json
{
  "asset_id": "asset://alerts/ready.wav",
  "media_type": "audio/wav",
  "data_b64": "UklGRiQAAABXQVZFZm10IBAAAAABAAEA...",
  "stamp_monotonic_ns": 8412345600000
}
```

## 停止运动

单设备普通运动停止操作。

```json
{
  "operation": "stop_motion"
}
```

## 夹持安全状态

`Clamp()` 将执行器速度归零并保持安全位置，不锁定后续命令。示例中的原因仅用于运行时日志，不是方法参数。

```json
{
  "operation": "clamp",
  "cause": "velocity_lease_expired"
}
```

## 停机锁定

`Halt()` 进入相同安全范围并锁定后续命令；原因仍是日志上下文。

```json
{
  "operation": "halt",
  "cause": "emergency_stop"
}
```

## 驱动结果

可能结果为 `accepted`、`rejected`、`failed`、`cancelled`、`timed_out`、`outcome_unknown`。拒绝表示未执行；不得默默改变单位、坐标系、关节顺序或媒体格式。

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

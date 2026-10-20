# 硬件本体接口

[English](../../../../../docs/eos/develop/interfaces/southbound/body.md)

[接口总览](../README_cn.md) · [世界输入](world_cn.md) · [驱动原语](primitives_cn.md)

本页展示原生 C++ HAL 的逻辑值；JSON 仅用于说明，不是 EOS 与 Agent 之间的序列化协议。驱动身份、健康和执行器状态属于本体接口；原始传感器输入和命令分别见世界接口与原语接口。

## 身份

驱动报告名称、版本、设备 ID 与设备类别。类别可以是机械臂、底盘、云台、相机、麦克风、夹爪或扬声器。

```json
{
  "name": "demo-camera",
  "version": "1.0.0",
  "device_id": "camera-0",
  "device_kind": "camera"
}
```

## 数据流声明

四类 feed 分别为几何、运动本体、原始传感器和健康状态；速率为零表示不产出该类数据。

```json
{
  "geometric": {"max_rate_hz": 10.0},
  "proprioception": {"max_rate_hz": 50.0},
  "sensor": {"max_rate_hz": 30.0},
  "health": {"max_rate_hz": 2.0}
}
```

## 健康

`alive` 表示驱动安全状态，`admitted` 只在桥接层完成身份、配置和 feed 检查后置为真。

```json
{"alive": true, "admitted": true}
```

## 底盘状态

位姿使用该底盘绑定约定的坐标系，不能统一假定为地图坐标系。

```json
{
  "pose": {"x_m": 1.2, "y_m": -0.4, "yaw_rad": 0.52},
  "velocity": {"x_m_s": 0.2, "y_m_s": 0.0, "yaw_rad_s": 0.1},
  "posture": "walk"
}
```

## 云台状态

角度与角速度使用弧度及弧度每秒。

```json
{
  "pan_rad": 0.2,
  "tilt_rad": -0.1,
  "pan_rate_rad_s": 0.0,
  "tilt_rate_rad_s": 0.0
}
```

## 机械臂状态

`joint_valid` 和 `pose_valid` 区分缺失读数与物理零值。末端位姿使用机器人描述中的机械臂基坐标系。

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

## 夹爪状态

位置和力可缺省；所持物体的关联由 EOS 感知层产生，不属于硬件遥测。

```json
{
  "state": "holding",
  "position_pct": 34.0,
  "force_n": 6.1
}
```

## 扬声器状态

这是输出设备状态。除 `state` 和 `underruns` 外均可缺省；遥测缺失时不应伪造零值。规范接口为 `eos/hardware/backend.h`。

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

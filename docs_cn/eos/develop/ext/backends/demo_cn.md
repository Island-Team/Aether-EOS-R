# Demo 后端

[English](../../../../../docs/eos/develop/ext/backends/demo.md)

本页描述可选 C++ demo adapter 及其 `eos.simulator.v1` 服务。默认 Rust v2 MuJoCo 路径使用 [v2 接口](../../../interfaces/mujoco_cn.md)，由 EOS 直接管理已验证的仿真器子进程。C++ adapter 是 EOS 对外部 simulator service 的 client；EOS 不包含 simulator world、physics engine 或安全认证环境。

## 隔离与可用性

所有 Demo adapter 共享 `<run>/simulator.sock` 的有界 client，并调用私有 `eos.simulator.v1.SimulatorService` 的 `Health`、`Execute`、`ReadTelemetry` 与 `ReadCameraFrame`；它们不构造、不启动、也不 tick world。外部 simulator process 拥有时钟和状态。

模拟器缺失或断开时，adapter 报告 unhealthy、拒绝命令且不返回 camera frame；后续调用会 重连。`eosd` 保持运行，不监管模拟器，并继续提供非 Demo 功能。

## 设备行为

- **base** 集成速度，并根据模拟器 raster collision map 执行导航。
- **gimbal**、**arm** 和 **gripper** 更新各自模拟 telemetry 与状态。
- **cam** 从模拟器读取有界 JPEG 及准确的 base/gimbal/scene 关联，然后进入正常 EOS perception-model 路径。
- 默认 `demo` profile 的 **mic** 与 **speaker** 使用 loopback stub；可选 `demo-host` profile 使用主机 ALSA 采集和原生 `aplay` 播放。

模拟器使用右手平面坐标系：X 正向向右、Y 正向向上、零 yaw 沿 X 正向、正 yaw 逆时针。 Raster 是碰撞权威。视觉来源线仅作为 trigger，绝不成为碰撞几何、label、detection 或 pick target。

当 simulator 使用 `camera_source = "scene"` 时，模拟器沿 `base.yaw + gimbal.pan` 发出水平射线，选择最近的正向来源线命中，通过全局 source library 解析其 `source_id`， 并按需读取/解码该规范文件。未命中或 source 不可用/非法时不返回 frame。EOS 独立校验模型输出；地图 metadata 绝不作为兜底 detection。

当使用 `camera_source = "system"` 时，simulator 改为将操作系统默认相机作为唯一 source， 并忽略 map source line；camera frame 仍通过 simulator IPC 到达 EOS 的 `demo` camera。 独立的 `demo-host` profile 则绕过 simulator camera path，由 `eosd` 通过 V4L2 直接采集 `/dev/video0`。

## Profile 与运行

`etc/eos/backends/demo.toml` 定义 Demo 设备。`demo-host` variant 保留同一 robot contract，同时选择 V4L2 摄像头、ALSA 麦克风和原生 `aplay` 扬声器。Profile limit 是配置示例，不是硬件安全限制。

历史 C++ stack 由启动器在 EOS 前启动已选 simulator 软件包。v2 则由 EOS 直接启动和停止仿真器子进程。当前软件包配置和就绪检查见 [MuJoCo 使用指南](../../../usage/mujoco_cn.md)。

## 诊断

Simulator lifecycle、IPC recovery、source selection、commit、hot reload 与有界故障使用共享 结构化日志策略。不记录正常 tick/frame 或 payload 数据，重复 disconnect/decode warning 会限速。 持久化、HTTP、WebSocket、校验与日志细节见[独立模拟器接口](../../../interfaces/external/simulator_cn.md)。

测试必须覆盖进程隔离、断开/重连、command/telemetry 边界、故障期间 camera 缺失、最近来源线 解析、source lazy access，以及重连后的正常模型处理。

**相关：** [模拟器接口](../../../interfaces/external/simulator_cn.md) · [Southbound 接口](../../../interfaces/southbound/driver_cn.md) · [硬件 runtime](../../../design/runtime/hardware-legacy_cn.md)

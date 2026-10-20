# Demo 后端

[English](../../../../../docs/eos/develop/ext/backends/demo.md)

Demo 后端是 EOS 到独立启动的 `eos-simulator` 进程的 adapter。它不是 EOS core mode、进程内 world、物理引擎或安全认证环境。Admission、dispatch、feed、model 与 northbound service 使用和其他后端相同的 EOS 路径。

## 隔离与可用性

所有 Demo adapter 共享 `<run>/simulator.sock` 的有界 client，并调用私有 `eos.simulator.v1.SimulatorService` 的 `Health`、`Execute`、`ReadTelemetry` 与 `ReadCameraFrame`；它们不构造、不启动、也不 tick world。`eos-simulator` 拥有单调时钟和状态。

模拟器缺失或断开时，adapter 报告 unhealthy、拒绝命令且不返回 camera frame；后续调用会 重连。`eosd` 保持运行，不监管模拟器，并继续提供非 Demo 功能。

## 设备行为

- **base** 集成速度，并根据模拟器 raster collision map 执行导航。
- **gimbal**、**arm** 和 **gripper** 更新各自模拟 telemetry 与状态。
- **cam** 从模拟器读取有界 JPEG 及准确的 base/gimbal/scene 关联，然后进入正常 EOS perception-model 路径。
- **mic** 与 **speaker** 保留配置的 host ALSA 和原生 `aplay` 行为；simulator media 不是 音频输入捷径。

模拟器使用右手平面坐标系：X 正向向右、Y 正向向上、零 yaw 沿 X 正向、正 yaw 逆时针。 Raster 是碰撞权威。视觉来源线仅作为 trigger，绝不成为碰撞几何、label、detection 或 pick target。

当 simulator 使用 `camera_source = "scene"` 时，模拟器沿 `base.yaw + gimbal.pan` 发出水平射线，选择最近的正向来源线命中，通过全局 source library 解析其 `source_id`， 并按需读取/解码该规范文件。未命中或 source 不可用/非法时不返回 frame。EOS 独立校验模型输出；地图 metadata 绝不作为兜底 detection。

当使用 `camera_source = "system"` 时，simulator 改为将操作系统默认相机作为唯一 source， 并忽略 map source line；camera frame 仍通过 simulator IPC 到达 EOS 的 `demo` camera。 独立的 `demo-host` profile 则绕过 simulator camera path，由 `eosd` 通过 V4L2 直接采集 `/dev/video0`。

## Profile 与运行

`etc/eos/backends/demo.toml` 定义 Demo 设备。`demo-host` variant 保留同一 robot contract， 但以 V4L2 capture 替换 simulator camera。Profile limit 是配置示例，不是硬件安全限制。

在 `eosd` 前后均可显式启动 `eos-simulator`，两个进程都不控制对方生命周期。配置的 `map_id` 从 `<state>/simulator/maps` 加载；更改 ID 需要重启 simulator，对 active map 的 commit 则原子 热加载。WebUI 直接连接 loopback `16004`；EOS northbound 不代理 simulator route。

## 诊断

Simulator lifecycle、IPC recovery、source selection、commit、hot reload 与有界故障使用共享 结构化日志策略。不记录正常 tick/frame 或 payload 数据，重复 disconnect/decode warning 会限速。 持久化、HTTP、WebSocket、校验与日志细节见[独立模拟器接口](../../interfaces/external/simulator_cn.md)。

测试必须覆盖进程隔离、断开/重连、command/telemetry 边界、故障期间 camera 缺失、最近来源线 解析、source lazy access，以及重连后的正常模型处理。

**相关：** [模拟器接口](../../interfaces/external/simulator_cn.md) · [Southbound 接口](../../interfaces/southbound/driver_cn.md) · [硬件 runtime](../../../design/runtime/hardware_cn.md)

# 准备硬件后端与驱动

[使用 EOS](README_cn.md) → [配置](configure_cn.md) → **后端** → [南向接口](../interfaces/southbound/driver_cn.md)

后端把 EOS 的设备命令与遥测映射到模拟器、主机设备或部署方驱动。一个 profile 声明语义设备、primitive 限制、feed rate 和各设备的后端；驱动或外部控制器负责物理行为，EOS 在[南向边界](../interfaces/southbound/README_cn.md)校验并释放命令。**没有机器人硬件时使用 demo，不需要真实驱动。**

默认 Rust v2 部署中，已验证的 MuJoCo 软件包向已应用图贡献 `eos.process.backend@v1.0` 及硬件节点。IsOS 解析软件包并启动 EOS；EOS 直接启动、监测和停止仿真器子进程。使用 `isos status` 查看部署，使用 `isos run` 或 `isos restart` 启动或替换部署。以下实体驱动与 C++ 配置步骤仅适用于可选侧链。

## 选择接入路径

| 路径 | 准备工作 | 详细说明 |
| --- | --- | --- |
| MuJoCo 模拟器 | 使用 `type = "mujoco"`；通过 IsOS 安装并管理外部软件包 | [MuJoCo 配置](mujoco_cn.md) |
| 主机麦克风、摄像头、扬声器 | 检查 ALSA/V4L2 设备权限、路由和工具 | [主机后端](../develop/ext/backends/host_cn.md) |
| ROS 2 机器人 | source ROS 与部署工作空间；在 Island 之外构建私有桥接包并声明驱动单元 | [ROS 2 后端](../develop/ext/backends/ros2_cn.md) |
| 原生厂商协议 | 实现 backend 与 descriptor，并在现场 profile 声明支持的操作 | [后端开发](../develop/ext/README_cn.md) |
| MuJoCo 包 | 安装并激活独立模拟器包 | [MuJoCo 运行](mujoco_cn.md) |

## 建立并选择 profile

按[本地配置](configure_cn.md)复制示例。`etc/eos/backends/demo.toml` 展示设备及 primitive 结构。真实端点、frame、音频路由和实测限值写入部署 profile 或图边缘适配器。`eos.toml` 中恰好一个 `[[mode.binding]]` 选择相对于 etc root 的文件及其 profile。

只声明驱动真正执行的 primitive。每个 `[[device.primitive]].limits` 区间应反映硬件权威限值；不得直接把 demo 限值用于实体设备。EOS 启动时拒绝无效绑定和不支持的声明，驱动仍须自己执行物理保护。

## 检查依赖与就绪状态

- 主机设备：确认 EOS 服务账户可访问 ALSA 或 V4L2 设备，且没有其他进程占用。
- ROS 2：构建、启动和诊断使用同一发行版及工作空间；核对 DDS domain、namespace、topic、消息类型、终态结果与取消语义。具体映射见[配置参考](configuration-reference_cn.md#ros-2-transport)。
- MuJoCo：EOS 启动已验证的直接子进程，并在向 IsOS 报告就绪前等待其 Unix 套接字。
- 原生新后端：按[扩展开发](../develop/ext/README_cn.md)增加构建目标、descriptor、解析准入和失败测试。

使用所选 `EOS_ETC` 和 `EOS_CONTRACTS` 启动 `./bin/eosd`，再启动 `./bin/eos-north-http`；查询 `http://127.0.0.1:16201/v1/eos/discover` 核对机器人、设备和 primitive。缺失或不健康时查看[调试与日志](../debug/README_cn.md)。发现接口不能证明真实运动链路；实体部署还须验证正常执行、拒绝、stop、estop、超时、取消、依赖故障与关闭。

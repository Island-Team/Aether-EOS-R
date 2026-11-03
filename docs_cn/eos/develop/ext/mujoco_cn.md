# 开发 MuJoCo 基准包

[English](../../../../docs/eos/develop/ext/mujoco.md) · [包开发](README_cn.md) · [运行](../../usage/mujoco_cn.md) · [设计](../../design/mujoco_cn.md)

`isos-mujoco-sim` 是位于 Island-eos 相邻目录的独立 C++20/CMake 仓库。它通过 `MUJOCO_ROOT` 链接 MuJoCo 3.16.0，EOS core 不链接 MuJoCo。`eos-mujoco-sim` 可执行文件在 `<run>/simulator.sock` 上实现现有的 `eos.simulator.v1.SimulatorService`，另在仅所有者可访问的 `<run>/benchmark.sock` 上提供私有基准服务。仓库地址为 [Island-Team/isos-mujoco-sim](https://github.com/Island-Team/isos-mujoco-sim)。

## 包结构

| 包内路径 | 职责 |
| --- | --- |
| `assets/generate_scene.py`、`assets/scene.xml` | 组合移动机械臂、摄像头、桌面、方块、托盘与随机障碍物 |
| `assets/menagerie/`、`assets/MODELS.md` | 固定版本的 UR5e 与 Robotiq 资源、来源和许可证 |
| `src/engine.*` | 固定步长物理状态、控制、IK、接触观测、渲染和评分 |
| `src/service.*` | EOS 模拟器 RPC 与私有基准 RPC 适配 |
| `proto/simulator.proto`、`proto/benchmark.proto` | EOS 模拟器契约副本与私有基准契约 |
| `tests/engine.cpp`、`tests/smoke.cpp` | 实体任务和服务层回归测试 |
| `LICENSE`、`assets/menagerie/*/LICENSE`、`assets/MODELS.md` | 包的 Apache-2.0 许可证、保留的模型许可证及模型来源 |

导入的 Menagerie 版本是 `cda5236f98383f6fbaa4820c6b18dba892cd087f`。保留模型许可证和源 XML；修改 `generate_scene.py` 后重新生成组合场景。EOS 模拟器契约如有变更，应同步更新包中固定的 `simulator.proto`。

## 集成边界

EOS 的 `etc/ext/mujoco/binding.toml` 仅声明底盘、机械臂、摄像头和夹爪。`mujoco` 描述符复用模拟器 IPC 适配，但由 MuJoCo 判断笛卡尔可达性和碰撞，而不使用 demo 机械臂的平面检查。运动 RPC 使用较长截止时间，健康与观测调用保持较短。包仅在实体目标达到时报告完成，否则报告明确失败；stop 和 halt 必须中断活动动作。

`ReadTelemetry` 与 `ReadCameraFrame` 返回测得的状态和渲染的 JPEG。当前摄像头固定为 `640 × 480`，使用无显示器 EGL。未知设备 ID 和命令应拒绝。只有当高层命令能够完成实体抓取并报告真实结果时，才可声明 `arm.pick`。

基准服务仅面向可信程序，可提供真实状态和种子控制；Agent 只接收 EOS 发现、观测和 primitive。准确 RPC 定义保留在[模拟器接口](../../interfaces/external/simulator_cn.md)及包内 protobuf 中。

## 验证

在包仓库运行 `cmake -S . -B build -DMUJOCO_ROOT=...`、`cmake --build build -j` 和 `ctest --test-dir build --output-on-failure`。引擎测试覆盖种子回放、运动完成、工作空间拒绝、stop/halt、连续 JPEG 和种子 42 的实体抓取放置；RPC 测试覆盖服务兼容、观测维度、JPEG、私有步进和健康。EOS 的配置、封装脚本、启动器及 demo 回归测试在 Island 构建树中运行。

`scene-view-check` 使用安装的 MuJoCo `compile` 工具检查组合 XML，不需要 EOS 或显示器。`tools/scene-view.sh` 先执行同样的检查，再打开 `simulate` 进行可视化调试。它检查场景加载与可见几何；按种子变化的位置和命令行为仍由引擎及 RPC 测试覆盖。

完整集成检查应启动包和 MuJoCo HAL 配置的 EOS，使用唯一 command ID 调用 EOS `Discover` 和 primitive，再从基准 socket 读取 `Score()`。回归测试保留脚本化示例，但不提供 Agent 策略。

## 种子 42 回归任务

确定性的种子 42 任务导航到桌边、夹住方块、抬升、用机械臂转移并在标记托盘上张开夹爪。包测试验证实体抬升和最终放置。通过 EOS `GatewayService` 与外部基准客户端运行时，导航、抓取和放置均得分。碰撞是独立指标；此任务在成功抓取期间仍记录机械臂与桌面的接触。

关节数组按 `etc/ext/mujoco/binding.toml` 的六个 UR5e 关节名称排列；`arm.movej` 使用 `reference = "absolute"`。每一行使用新的 EOS command ID。基准程序在夹爪闭合后、抬升后分别调用 `Step(200)`，张开后在引擎测试中调用 `Step(300)`，在 EOS 集成运行中调用 `Step(500)`，使方块稳定。

| 顺序 | EOS primitive | 目标 |
| --- | --- | --- |
| 1 | `base.navigate` | 相对 x `0.6` m、y `0`、yaw `0` |
| 2 | `arm.movej` | `[2.7368, -0.4246, -1.1208, -0.4245, 4.7113, -2.1504]` |
| 3 | `arm.movej` | `[2.7604, -0.2680, -1.1200, -0.2037, 4.6546, -2.1504]` |
| 4 | `gripper.motion` | `action = "close"` |
| 5 | `arm.movej` | 重复第 2 行并抬升 |
| 6 | `arm.movej` | `[3.3459, -0.2874, -1.1062, -0.2623, 4.6620, -2.1504]` |
| 7 | `gripper.motion` | `action = "open"` |

直接通过 EOS 运行时，三个目标布尔值均为 true，用时 `7.32` 模拟秒，记录 `11` 次接触事件。这些目标仅是种子 42 的回归示例，不是适用于其他种子的 Agent 策略。

**相关：** [EOS 扩展指南](README_cn.md) · [模拟器接口](../../interfaces/external/simulator_cn.md) · [硬件运行时](../../design/runtime/hardware-legacy_cn.md)

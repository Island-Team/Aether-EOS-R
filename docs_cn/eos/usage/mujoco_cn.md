# 在 EOS v2 中运行 MuJoCo

[English](../../../docs/eos/usage/mujoco.md) · [节点 IDL](../interfaces/node-contributions_cn.md) · [硬件契约](../interfaces/hardware_cn.md) · [模拟器接口](../interfaces/mujoco_cn.md) · [R06 停止契约](../interfaces/simulator-stop_cn.md)

`island/mujoco-sim@0.1.1` 是由 IsOS 管理的 MuJoCo 模拟器包，EOS 将它作为直接子进程运行。该包提供可执行文件、场景及贡献的进程类型；所选相机、底盘和机械臂使用通用 EOS 硬件类型并锁定适配器包。模拟器使用仅所有者可访问的 Unix 套接字；EOS 拥有硬件适配器和 Dispatcher。R05 EOS 严格客户端启动的子进程不含历史 gRPC 执行器或 benchmark 监听器；限定范围的跨所有者 R05 资格验证已通过；两个套接字证明同一 generation 的 `strict-stop-v1` 后，才公开受管理运动。独立直接启动的评测器可在受管理部署之外使用 benchmark 套接字；EOS 与 EGIA 从不接收真实状态。

## 准备与运行

先构建 Aether Rust 可执行文件和 IsOS，再从[模拟器仓库](https://github.com/Island-Team/isos-mujoco-sim)构建 v2 包。将 `MUJOCO_ROOT` 指向包含 `lib/libmujoco.so.3.16.0` 的 MuJoCo 3.16.0 安装目录。链接本地包并启动 IsOS 工作台：

```sh
export MUJOCO_ROOT=/absolute/path/to/mujoco-3.16.0
isos init --mode dev
isos pkg link island/mujoco-sim 0.1.1 /absolute/path/to/bundle --manifest /absolute/path/to/bundle/release.json
isos version check
isos serve
```

在本仓库构建根 Cargo 工作区与 `ui`，在 IsOS 中配置仓库内产物路径，然后运行 `isos version check`。MuJoCo 软件包仍在仓库外部。

`isos init` 创建配置后退出；让 `isos serve` 保持前台运行 manager 和工作台。在 IsosUI 的 Graph 页面选择已安装的 `sim-hardware` 节点集，选择 `pick-place` 或 `navigation`，预览节点，再 Validate 和 Apply 修订。该旧版 `0.1.0` 包进程节点使用 `core=eos`、`layer=process`、`group=mujoco`；硬件节点使用通用相机/底盘/机械臂 group。统一的 `0.2.0` 图改用 `isos/eos_process` 和 `isos/eos_hardware` 位置；完整所属检查与首次使用回放通过前，迁移 Apply 仍拒绝写入。预设把相机连接到通用 EOS 相机门节点，但不会创建旧版 `isos.ui.vision@v1.0` 节点或 `preview` 连线。安装和预览不会改动已应用的图；Apply 才会改动。使用 **Run** 启动该修订，或在第二个终端运行 `isos run`。如果旧修订仍在运行，请明确执行基于已应用图的重启。

对于已就绪的受管理 MuJoCo 子进程，例行 IsOS Stop 或 Restart 使用 EOS 托管关闭栅栏。只有双执行器停止、子进程回收和清理都得到证明，选中回执 confirmed 才解除这个临时栅栏；它不会创建或清除真正的 `safety.estop` 锁存及既有机器人抑制。回执 unknown 时运动继续禁止。发出新运动前，检查 IsOS 所选终态和新运行的规划器可见 binding。既有急停或结果不明的运动仍须遵循各自的所有者恢复流程；不得清除 `.control` 或重放旧命令 ID 来使 Restart 通过。参见[托管关闭契约](../interfaces/simulator-stop_cn.md#与急停分离的例行关闭状态)。

EOS 只读取已应用的图。IsOS 解析并验证可执行文件及场景；EOS 启动 MuJoCo，只连接图中声明的适配器。R06 停止路径在子进程终止前栅栏阻断已准入执行器；证明失败时报告 unknown，尚未覆盖的实时故障见[停止设计](../design/runtime/simulator-stop_cn.md)。包缺失、链接资源发生变化、场景无效或类型端口不兼容，都会在硬件启动前使准入失败。dispatcher 仍是底盘和机械臂动作的唯一命令入口。

[历史 TOML 图](../../../examples/v2/mujoco-isos.toml)是只读 `1.0.0` 兼容性样例。它的 `scene = "default"` 和 `@v1.0` 节点不是新的 `0.1.1` 包部署。首次使用请选用上方已验证节点集；迁移已有图须通过 IsOS 显式且按修订校验的迁移流程。

**R05 EOS 首次使用行为：** EOS 带 `--managed-strict-socket` 启动已验证子进程，不启用历史 gRPC 或 benchmark 监听器。报告就绪前，逐一探测已声明的相机/底盘/机械臂设备，要求 `version:"2.0.0"`、回显请求 ID、`ok:true`、`accepted:true` 和包含 `strict-socket-v1` 的 `features`。仅有套接字文件只是启动进度。特性、设备或相机渲染不可用时，先看当前运行的首条 `eos_start_failed` 及后端节点状态。就绪后检查相机采集与底盘/机械臂观测。只有两个套接字证明同一 generation 的 `strict-stop-v1` 终端栅栏后才公开受管理运动 primitive；就绪本身不能确认后续物理停止。之后若已准入运动产生 `OutcomeUnknown`，也不能自动重发。

若选定图声明底盘或机械臂，而配套停止套接字仍在启动，EOS 会等到严格证明通过后才报告就绪。若到启动期限仍不可用，请查看本次运行的 `eos_start_failed` 与后端日志；此时没有运动准入。只有相机的图可在没有停止 generation 时保持只读。

## 查看相机

预设中的 EOS 相机门节点是将 EOS `CameraFrame` 转为有界 `VisionFrame` 的声明来源。IsosUI Vision 面板可用时，在面板中选择这个活跃且类型兼容的门节点作为预览来源；预设本身不提供 UI 节点绑定。浏览器通过只读 WebSocket 接收 JPEG 帧并显示实际送达帧率。初始目标帧率是相机声明的帧率；Vision 设置可以限制 FPS、图像尺寸和 JPEG 质量。之后由 UI 所有者建立的来源绑定也须使用门节点类型化输出，不能绕过 EOS 命令准入。

用 `isos status` 比较已保存和正在运行的修订，并用 `isos logs eos` 或 IsosUI 日志面板查看启动及相机故障。按 `run_id` 找到当前运行的首条 `eos_ready` 或 `eos_start_failed`；`eos_ready` 确认 EOS 本地就绪，IsOS 状态才确认部署就绪。Rust EOS 生产者向 stderr 写有界 JSONL。IsOS 归属 `data/logs/<deployment>/` 下的部署日志库；受管理的 MuJoCo 原始输出只写入 `owner/eos/mujoco.log`，不是共享 EOS 事件。有关联 ID 的停止在 `eos_stopping`、任何 `eos_stop_unconfirmed` 和 `eos_stopped` 上使用同一 `correlation_id`。`isos deployment stop` 只停止图进程，`isos stop` 则关闭整个 manager 和工作台。

## R06 受管理停止首次使用

**所有者路径已实现，限定范围的跨所有者验证已通过；原始 R06 条目与限定范围 M3 已验收。** 使用 `aether-runtime --capabilities` 探测准确的 `managed-stop-v1` 特性。该 token 只声明 EOS 支持此契约，不代表停止结果。 尝试活跃运动前，先确认 EOS 在两个 MuJoCo 套接字均发现 `strict-stop-v1`，再尝试活跃的运动 binding。检查 IsOS 活动修订与 EOS capability 可用性，提交带命令 ID 的有界 primitive，并以关联操作结果为准；不重发结果不确定的运动。MuJoCo 的物理停止覆盖整台机器人，因此不发布 `motion.stop`；可用时使用机器人范围的 `robot.stop`。部署 Stop 使用工作台或运行 `isos deployment stop`；IsOS 先停止 EGIA 等依赖者，再停止 EOS，EOS 则关闭准入、通过 MuJoCo 配套停止 lane 获取底盘与机械臂各自的保持证明，然后终止子进程、排空其进程组并回收 leader。按当前 `run_id` 检查 IsOS 进程状态：`terminal.outcome:"confirmed"` 需要选中 EOS 原始管道的终端凭据及干净退出；`unknown` 表示物理停止、后代排空或清理无法证明。单独的 `eos_stopped` 只是诊断，PID 消失不能证明确认。停止 unknown 时保持运动禁用，遵循所有者恢复流程后才可再次 Run 或 Restart。就绪前的 GUI/渲染故障属于启动失败；就绪后渲染阻塞仍可能通过独立 lane 得到 confirmed 停止。子进程崩溃、停止超时、幸存后代或强杀产生 unknown 停止。跨日志轮转查找首次事件与停止关联；部署停止后，旧图仅可经 IsOS 显式修订校验迁移。

[R06 资格记录](../design/runtime/simulator-stop_cn.md#已合并所有者与限定范围资格状态2026-11-02)锁定经审查的所有者源码及选中原始管道输出。两个全新无界面 IsOS 场景在受治理底盘与机械臂运动后确认双部分停止。停止套接字丢失、子进程/提供者死亡及强杀返回 unknown；无响应停止和其他子进程管道上的伪造凭据在管理器重启后仍为 unknown，并阻止 Restart。阻塞的相机采集可能完成；关闭会封闭新的采集和发布，其 worker 结果受界限约束且不能延迟配套停止通道。这些重放未覆盖实时物理部分证明、Stop 进行中管理器死亡或就绪后 GUI 渲染阻塞。原始 R06 所有者条目与限定范围 M3 完成条件已由[M3 记录](../../island/m3-qualification_cn.md)验收；更广泛的实时用例和 M5 仍未完成。

## 基准测试隔离

包内基准程序在受管理 EOS 子进程之外使用单独直接启动的模拟器，进行种子重置、步进、读取真实状态和评分。受管理严格子进程拒绝该套接字，因为 Reset 和 Step 会在 Dispatcher 外改变引擎。协议见[包仓库](https://github.com/Island-Team/isos-mujoco-sim)。EOS 客户端只使用图中声明的相机采集、观测和经 Dispatcher 准入的动作；它们不会得到基准测试状态。修改模拟器或节点声明后，运行包内 `ctest --test-dir build --output-on-failure` 以及 IsOS/EOS 集成测试。

**相关：** [IsOS 包流程](../../../docs/isos/usage/packages.md) · [EOS 硬件设计](../design/runtime/hardware_cn.md)

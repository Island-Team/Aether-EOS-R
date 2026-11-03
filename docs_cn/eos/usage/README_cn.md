# 运行 EOS：首次使用

[文档](../../README_cn.md) → [EOS](../README_cn.md) → **使用 EOS**。另见[架构](../../island/architecture_cn.md)、[接口目录](../interfaces/README_cn.md)和[构建说明](../develop/build-and-run_cn.md)。

本指南按一次部署的实际使用顺序编排。当前 Rust 读取器接受既有 `1.0.0` 图和 `@v1.0` 类型。新 `0.1.0` 图、`@v0.1` 类型、选择器和连线调度仍是**目标设计**；在[实现待办](../maintenance/todo_cn.md)完成前使用当前图。IsOS 负责图准入、软件包和部署；EOS 负责已准入设备及指令。

## 1. 构建

先按[快速开始](../../../README_cn.md)安装依赖，再在本仓库执行 `./build.sh`。IsOS 与 IsosUI 分别在其仓库构建。外部 `island/mujoco-sim@0.1.1` 包由 IsOS 安装。ARM64 环境见[配置指南](arm64_cn.md)。

## 2. 安装软件包并选择节点

当前 IsOS 默认配置文件为 `config/isos.toml`，也可用 `ISOS_CONFIG` 指向其他文件。在其中设置 EOS 二进制和 UI bundle 路径。注册表安装和精确版本锁定见 [IsOS 首次使用指南](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/usage/get-started_cn.md)。安装发布版只提供已验证的节点类型和资源，不会启动它们。

在部署图中选择 MuJoCo 场景及相机、底盘或机械臂节点。IsOS 将包和场景 ID 解析为已验证路径。当前流程见 [MuJoCo 配置](mujoco_cn.md)，目标选择器元数据见[节点贡献接口](../interfaces/node-contributions_cn.md)。独立模型服务使用 IsOS 的服务配置和共享状态，不属于该部署图。

## 3. 校验并应用

```sh
isos config validate <graph-file>
isos config apply <graph-file>
isos version check
```

校验在激活前报告 schema、节点、包和路由错误。当前应用会为部署写入生成配置；目标带修订检查的应用将发布不可变 EOS/EGIA 快照。EOS 只读已应用图。见 [IsOS 图接口](https://github.com/Island-Team/isos/blob/master/docs_cn/isos/interfaces/node-graph_cn.md)和 [EOS 硬件接口](../interfaces/hardware_cn.md)。目标预检将实际图和精确包版本与组件声明的支持范围比较。

## 4. 启动并确认就绪

在不同终端运行 `isos serve` 和 `isos run`；后者保持附着。用 `isos status --json` 查看部署和服务状态。IsOS 启动 EOS；EOS 准入节点、将 MuJoCo 作为直接子进程启动、连接仅所有者可访问的套接字并报告就绪。故障反映在状态与日志中。EOS 先停止已准入执行器，再终止 MuJoCo。见[基础生命周期](../design/runtime/foundation_cn.md)。

## 5. 检查与控制

R01 目标证据路线在启动后按[检查 EOS 证据连线](routed-edges_cn.md)操作；该页区分已配置图连线和实时传递。

在 IsosUI 检查图和监控。当前 UI 尚未显示连线优先级/频率或目标浮动工作区。当前 Unix CLI 用 `./bin/eos-cli --socket <runtime-socket>` 打开，套接字来自已应用部署。`discover` 列出已准入节点；`health <path>` 和 `capture <path>` 检查节点；`quit` 只退出客户端。见 [CLI 命令](repl_cn.md)及[控制接口](../interfaces/control_cn.md)。所有已准入执行器指令均通过 EOS dispatcher。

## 6. 查看日志和排障

先用 `isos status --json` 识别当前部署，再用 `isos logs all --lines 200` 查看最近的就绪或故障行。`isos logs eos --lines 200` 把当前输出缩到 EOS。**目标** [Rust JSONL 契约](../interfaces/events_cn.md)中，启动前 Apply／包失败属于 IsOS 管理器会话 ID；启动后应先使用部署 run ID，再按来源、节点或关联 ID 筛选。IsOS P07 将加入游标读取和轮转恢复，确保首个事件仍可找到。用一个关联 ID 对照 IsOS、EOS、MuJoCo 和 EGIA，再检查权威状态及图修订。[调度设计](../design/runtime/scheduler_cn.md)定义未来连线节流、溢出与停止事件。

## 7. 停止、重启或升级

`isos deployment stop` 停止当前部署；`isos restart` 运行选定修订；`isos stop` 同时关闭 manager 和其管理的服务。升级前检查精确包版本与图兼容性；必要时显式迁移，再校验并应用新修订。历史 `1.0.0` 图字节及 `@v1.0` 身份在带修订检查的显式迁移前保持不变。见[软件包指南](packages_cn.md)及[实现待办](../maintenance/todo_cn.md)。

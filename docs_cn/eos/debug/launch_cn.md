# 启动与管理本地 EOS

[使用 EOS](../usage/README_cn.md) · [配置参考](../usage/configuration-reference_cn.md) · [调试与日志](README_cn.md)

本页记录显式启用的 C++ checkout 启动器及其历史 stack 格式。默认 Rust v2 部署使用 `isos serve` 运行 manager，`isos run` 启动图子进程，`isos deployment stop` 只停止这些子进程。独立模型类服务使用单独的 IsOS 服务文件，不会随图自动启动。`launch.sh` 只打开 Rust `eos-cli`，其[当前命令集](../usage/repl_cn.md)不同于旧版 C++ REPL。以下旧版启动器仅在显式构建 C++ 侧链后可用。

## 命令

| 命令 | 效果 |
| --- | --- |
| `./bin/island-launch eos start` | 启动缺失的驱动与托管服务。 |
| `./bin/island-launch eos stop` | 停止托管服务，保留驱动。 |
| `./bin/island-launch eos restart` | 重启托管服务，保留驱动。 |
| `./bin/island-launch backend stop` | 停止整个服务图，包括驱动。 |
| `./bin/island-launch backend restart` | 按依赖顺序重启整个服务图。 |
| `./bin/island-launch eos status --json` | 查看已选/运行中的 stack、单元状态和 north HTTP URL。 |
| `./bin/island-launch eos logs` | 查看已声明单元的近期 journal。 |
| `./bin/island-launch profile use PATH` | 选择 `stack.toml` 或包含该文件的目录；`default` 选择仓库默认 stack。 |

选择操作不会改变正在运行的单元，需要重启才能生效。更换驱动时使用 `backend restart`；旧图驱动还在运行时，`eos restart` 会拒绝切换到另一 stack。CLI 输出沿用 *profile* 一词，实际选择的是 runtime stack。

## Stack 与模块

`etc/runtime/stack.toml` 指定 EOS 配置、核心服务，以及要加载的每个外部模块。只安装包不会把它加入服务图；必须在 `modules` 中显式列出模块路径。自定义 stack 完整替换默认 stack，不与默认服务表合并。

可选的 `package_root` 指向非默认的 `island-pkg` 软件包目录（绝对路径或相对于 stack）。软件包模块必须与该目录的 `isos.lock` 中的准确 ID 和版本匹配，且安装内容未变。

```toml
version = 1
eos_config = "../eos/eos.toml"
modules = ["../ext/simulator/module.toml", "../ext/north-http/module.toml"]

[[service]]
name = "eosd"
role = "managed"
command = ["{root}/bin/eosd", "--config", "{eos_config}"]
ready_tcp = "127.0.0.1:16200"
```

每个 `etc/ext/<name>/module.toml` 的模块 ID 必须与目录名一致。外部发布还可记录包 ID 与版本。禁用模块后，其服务与 binding 声明不会进入服务图。

```toml
version = 1

[module]
id = "mujoco"
enabled = true

[[service]]
name = "mujoco-sim"
role = "managed"
command = ["{package_file:bin/eos-mujoco-sim}", "--socket", "{run}/simulator.sock"]
ready_unix = "{run}/simulator.sock"

[[binding]]
backend = "backends/mujoco.toml"
profile = "benchmark"
requires = ["mujoco-sim"]
eos_after = ["mujoco-sim"]
```

`[[binding]]` 的 `backend` 相对所选 `eos_config` 所在目录解析，并须与 EOS `[[mode.binding]]` 的文件及 profile 精确匹配。每个选中的 EOS binding 都需要一项 runtime 声明。`requires` 指定必须启用的服务；`eos_after` 将依赖添加到 `eosd`。启动器不再根据 backend 文件名或设备类型推断模拟器、ROS 桥，相关进程和依赖必须显式声明。

服务名在 stack 和已启用模块之间必须唯一。`role` 为 `managed` 或 `driver`，驱动只能依赖其他驱动。`command` 是以绝对可执行路径开头的参数数组，不经 shell。`after` 指定启动依赖。服务可声明一个 `ready_tcp` 或 `ready_unix` 端点。可选 `timeout_ms` 范围是 1–300000，`env` 是字符串表，`enabled` 默认为 true。启动器拒绝依赖环、缺失或禁用的依赖、重复端点、端点占用和就绪失败。命令及端点支持 `{root}`、`{run}`、`{bundle}`、`{eos_config}`、`{sim_config}`、`{north_http_config}` 占位符。

启动器校验进程图和所选 binding；对于软件包模块，还校验 `isos.lock` 中的准确版本及安装内容摘要。`eosd` 在准入 binding 前重复校验。图变更在重启后生效；旧 `launch.toml` bundle 会收到迁移错误。

## 进程所有权与排障

`driver` 单元在 `eos stop` 后保留，`managed` 单元则停止；`backend stop` 停止两者。启动器确认已停止单元的 cgroup 和就绪端点均已释放。它不会接管无关进程，也不会按进程名杀进程。`eosd` 负责设备准入和硬件 dispatch；桥与驱动仍是独立进程。

排障时查看 `eos status --json`、`eos logs`、已选 `stack.toml`、引用的模块及 EOS binding。核对每项服务的可执行文件与就绪端点。EOS-only 启动失败时会回滚托管服务，但驱动可能继续运行，可用 `backend stop` 清除；完整 backend 启动失败时也会回滚驱动。

# IslandOS Aether · EOS

[English](README.md) · [文档目录](docs_cn/README_cn.md) · [Island 与 Aether](docs_cn/island/README_cn.md)

**Island Team** 开发 **IslandOS Aether**，将智能体决策与机器人执行分开。**EOS（Embodied OS，具身操作系统）** 的默认 v2 运行时以 Rust 实现：校验节点图、提供硬件观测，并在命令到达适配器前执行统一准入。旧 C++ 技能与工作流栈保留为未启动的侧链。独立的 **EGI Agent（EGIA）** 在共享节点图中有自己的 core，其执行不属于当前 MVP。[Island 总览](docs_cn/island/README_cn.md)介绍相关仓库、[网站](https://www.os-island.com)、软件包客户端和启动工具。

## 阅读顺序

1. [Island 产品与仓库地图](docs_cn/island/README_cn.md)：先了解 Aether、Agent、EOS、网站和工具。
2. [使用 EOS](docs_cn/eos/usage/README_cn.md)：构建 EOS，通过 IsOS 配置 MuJoCo 或机器人后端，并管理启动和软件包。
3. [开发 EOS](docs_cn/eos/develop/README_cn.md)：找到负责某项行为的模块及其边界。
4. [接口](docs_cn/eos/interfaces/README_cn.md)：查看模块之间的契约。
5. [运行时设计](docs_cn/eos/design/runtime/README_cn.md)：排查故障和修改算法时阅读内部流程。

## 构建 EOS

安装 Rust、Cargo、CMake 3.22+ 与用于 CXX 适配器的 C++17 编译器，然后从仓库根目录构建默认 Rust v2 进程组：

```sh
./build.sh
```

通过 `bin/eos-config validate --file etc/eos/backends/mock-cxx.toml` 可在打开设备前校验图。使用 `isos init` 创建配置，`isos serve` 运行 manager，`isos config apply <graph>` 应用图，`isos run` 启动部署；`isos deployment stop` 只停止图子进程，`isos stop` 同时关闭 manager。[MuJoCo 图模板](examples/v2/mujoco-isos.toml)包含模拟器、EOS、HTTP 桥接与配置服务节点。源码与安装路径、冒烟命令及实体硬件限制见 [v2 构建与启动指南](docs_cn/eos/develop/build-and-run_cn.md)。旧 C++ 构建仅在显式执行 `./build.sh --cpp-side-chain` 时启用。

## Rust 工作区结构

虚拟 Cargo 工作区采用与 `../Island` 一致的顶层 `crates/` 布局。十个 crate 均为 `0.2.0`，包名以 `aether-` 开头：

| Crate | 职责 |
| --- | --- |
| `aether-common`、`aether-node-profile` | 有界时间与 I/O 工具；节点类型注册和整图验证。 |
| `aether-hal`、`aether-mujoco` | 类型化相机、底盘和机械臂接口；隔离的 CXX mock 和 MuJoCo 适配器。 |
| `aether-dispatcher`、`aether-runtime` | 执行器唯一命令入口、持久化结果、适配器所有权和控制进程。 |
| `aether-control`、`aether-http-bridge`、`aether-config-server`、`aether-tui` | 有界客户端传输、回环 HTTP、只读图服务和 REPL。 |

Rust 可执行文件为 `aether-runtime`、`aether-profile`、`aether-http-bridge`、`aether-config-server`、`aether-tui`。公开的 `eosd`、`eos-config`、`eos-north-http`、`eos-cli` 命令启动这些程序。IsOS 管理进程图，默认使用 `aether-profile` 校验；覆盖变量为 `ISOS_AETHER_PROFILE_BIN`。参见[构建与启动指南](docs_cn/eos/develop/build-and-run_cn.md)和[节点接口](docs_cn/eos/interfaces/hardware_cn.md)。

`src/eos/` 保留需要显式启用的 C++ 硬件和旧运行时支线；默认 Rust 构建与安装不包含它。仍使用该支线的调用者可查阅 [EOS 文档](docs_cn/eos/README_cn.md)。

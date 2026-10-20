# IslandOS-Aether — EOS

<div align="center">

**EOS — Embodied OS（具身操作系统）**

*EGIA—EOS 架构的执行与硬安全层*

`C++17` · `CMake 3.22+` · `gRPC` · `ProtoJSON` · `可选 ROS 2 Humble/Jazzy`

[English](README.md) · [文档](docs/eos/README.md) · [网站](https://os-island.com)

</div>

EOS 是 IslandOS Aether 的原生运行时，负责机器人设备集成、具身感知资源投影、命令准入和执行，并拥有指令到达硬件前最后一道硬安全边界。EOS core 只提供 protobuf gRPC；独立 north HTTP 为 REPL 与 WebUI 提供 REST/SSE。

本仓库以适合并入产品树的 `src/eos/` 布局保存 EOS 历史，并在 `share/contracts/` 中维护 Agent 与运行时共用的规范线上契约，供独立构建和合并构建使用。

## 当前内容

EOS 由 `src/eos/runtime/` 的初始化、硬件准入、感知、模型调用、命令分发、技能和 northbound 模块组成。初始化模块解析严格配置并加载不可变 schema；硬件模块绑定一个 profile；感知模块投影只读设备与场景状态；模型模块调用有界的外部推理服务。所有设备命令经由 dispatch 进行 schema、范围和安全状态检查。

`src/eos/ext/` 提供独立的 north HTTP 服务、诊断 REPL，以及 demo、ALSA、可选 ROS 2 和部署自有的设备适配器。公开硬件与技能接口位于 `src/eos/include/eos/`；Agent 与 EOS 的 protobuf 和 JSON Schema 契约位于 `share/contracts/`。相关模块和接口的详细说明见 [EOS 文档索引](docs_cn/eos/doc_index_cn.md)。

## 构建与测试

需要支持 C++17 的编译器、CMake 3.22 或更高版本、Boost 1.74 或更高版本、spdlog、`protoc`，以及带有 `protobuf==3.20.3` 的 Python 3。测试还需 GoogleTest。使用原生 `aplay` 扬声器后端的部署还须安装 `alsa-utils`。对于 protobuf、gRPC、toml++、nlohmann/json 和 JSON Schema validator，CMake 会优先使用满足版本要求的系统包，否则获取已固定版本的可移植依赖。

先运行 `uv sync --locked --group contracts` 创建 `.venv`，或在其他 Python 虚拟环境中安装 `protobuf==3.20.3`。`./eos-build.sh` 会自动选用已有的 `.venv/bin/python`。

```sh
cmake --preset dev -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset dev -j
ctest --preset dev
```

不构建测试时使用：

```sh
cmake --preset default -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset default -j
```

构建产物统一位于 `build/` 下；默认与开发预设共用启动脚本所期望的 `build/aether`，ROS 2 预设按架构和发行版使用独立目录。

现有 `default` 和 `dev` 预设仍是主要的 x86 工作流。原生 ARM64 构建需显式使用 `arm64` 或 `arm64-dev` 预设，并写入独立的 `build/aether-arm64`；详见 [ARM64 运行指南](docs_cn/eos/usage/arm64_cn.md)。

先运行 `cp etc/eos/eos.example.toml etc/eos/eos.toml` 创建不会提交的本地配置。`./eos-build.sh` 会读取该文件选择的 backend profile，并自动判断是否构建 ROS 2。ROS 2 profile 根据已加载环境中的 `ROS_DISTRO` 选择 Humble 或 Jazzy 预设，并按架构和发行版使用独立目录。新的构建目录会在已安装 Ninja 时优先使用 Ninja。`./eos-build.sh --clean` 只清理并重建当前选择对应的构建目录。

### ROS 2

可选桥接层面向 ROS 2 Humble、Jazzy 和标准 ROS 接口。构建前请加载 ROS 安装环境与机器人工作区：

```sh
source /opt/ros/humble/setup.bash
source /path/to/ros2-workspace/install/setup.bash
./eos-build.sh
```

## 运行

公共启动脚本会根据自身位置解析路径，因此可从任意当前目录调用：

```sh
./bin/eos-simulator
# 在另一个终端中：
./bin/eosd
# 在另一个终端中：
./bin/eos-north-http
# 在另一个终端中：
./bin/eos-repl
```

所有 public wrapper 都可从任意目录启动，并可用绝对 `ISLAND_BUILD_DIR` 选择 build tree。EOS 常用参数为 `--config`、`--etc`、`--contracts`、 `--skills-dir`、`--host`、`--grpc-port` 与 `--log-level`；north HTTP listener、 upstream、CORS 与 limits 位于 `etc/eos/north-http.toml`。

默认使用仓库自带的 `share/contracts`。可通过 `EOS_CONTRACTS` 或 `--contracts` 选择该规范契约树的其他绝对路径副本。默认选择无需硬件的公开 `demo` profile。部署侧可选择自有的真机绑定；可选 ROS 2 后端支持机械臂、夹爪和摄像头，原生 `aplay` 后端经系统 ALSA 路由播放已验证的 PCM WAV。详情见 [Aplay 扬声器后端](docs_cn/eos/develop/ext/backends/host_cn.md#aplay-扬声器后端)。

### 配置自有硬件

EOS 将进程设置和硬件设置分开：`etc/eos/eos.toml` 只选择一个后端文件及其中的 profile；`etc/eos/backends/*.toml` 描述设备、primitive、调用范围、数据频率和传输端点；`src/eos/ext/backends/` 中的可复用适配器负责转发；厂商协议和物理设备由部署侧驱动或 ROS graph 管理。

接入现有后端时不需要修改 C++：

1. 以 `etc/eos/backends/demo.toml` 为结构示例，将所需设备声明复制到部署侧 TOML。
2. 为设备声明语义 `class`、`part_id`、支持的 primitive、真实可调用范围和 feed 上限。
3. 将假的 `backend.type = "demo"` 替换为已有的真实适配器；机械臂、夹爪和相机可使用 `ros2`，系统扬声器可使用 `aplay`。ROS domain、topic、frame、joint 顺序以及 ALSA 路由等对齐信息全部写在对应设备的 backend 表中。
4. 在 `etc/eos/eos.toml` 的 `[[mode.binding]]` 中选择部署文件和对应 profile。仅在确实需要 operator primitive 时开启 `expose_operator`。
5. 启动部署侧驱动、ROS graph 和模型服务后启动 EOS。EOS 会在开放网关前验证整个 profile，再通过 `eos-repl` 或 Discover 接口检查识别到的设备和 primitive。

完整 TOML 示例、配置流和安全边界见英文主文档的 [Configure a robot](README.md#configure-a-robot)。配置中的范围用于准入、发现和分发检查，不能代替控制器、URDF、碰撞保护和硬件限位。

EOS 默认只在 `127.0.0.1:16003` 提供 gRPC；显式 simulator 使用 `16004` 和私有 socket；独立 north HTTP 在 `127.0.0.1:16005` 提供完整 REST/SSE， REPL 与 WebUI 都通过它访问 core。`eosd` 不监管任何外部进程； 开发可用 `eos-launch.sh` 监管 simulator → EOS → north HTTP → REPL。完整 route、 SSE、error、request ID 与 security 见 [HTTP 契约](docs_cn/eos/develop/interfaces/external/http_cn.md)。

## 开发

优先通过 backend profile 接入新硬件。只有现有后端无法表达传输方式或设备语义时才新增代码：在 `src/eos/ext/backends/<type>/` 实现 `eos::hardware::Backend`，在 `runtime/hardware` 增加类型准入和工厂，并让适配器从 profile 读取所有端点、joint、frame 和范围。不要按型号或厂商在运行时代码中选择硬件。

新增后端须包含 fake driver 单元测试、配置解析的成功与拒绝测试、registry 准入测试，以及新命令或遥测路径的 dispatch/body 测试。仓库单元测试不得依赖真机。开发命令和完整检查清单见英文主文档的 [Development](README.md#development) 与 [southbound body interface](docs/eos/develop/interfaces/southbound/body.md)。

## 合并到产品树

本仓库按保留历史的合并方式组织：

- EOS 内容固定在 `src/eos`、`tests/eos`、`etc/eos` 和 `docs/eos`。
- 根 CMake 会按实际存在情况发现 EOS 与未来同树的 EGIA 组件。
- Agent 与 EOS 共用 `share/contracts` 中的同一套跨进程规范契约。
- 公共命令位于 `bin`，原生可执行文件安装到私有的 `libexec/island` 目录。

架构、配置、接口、模块参考和发布说明见 [EOS 文档索引](docs/eos/README.md)。

贡献者名单见 [Contributors](CONTRIBUTORS.md)。

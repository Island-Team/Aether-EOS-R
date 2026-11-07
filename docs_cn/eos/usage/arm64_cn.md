# ARM64 running guide

本页描述 ARM64 上可选的 C++ 侧链。默认 EOS v2 运行时使用 Rust；其构建与 IsOS 部署流程见 [v2 构建与启动指南](../develop/build-and-run_cn.md)。C++ 侧链没有限定 x86 的源码或编译器参数。这里的 ARM64 路径是在目标机原生构建侧链及所有原生依赖。`x86_64` 与 `arm64` preset 分别使用 `build/aether` 与 `build/aether-arm64`，不共享目标文件或 CMake 缓存。

## Supported path

当前平台基线为搭载 NVIDIA GPU 的 Ubuntu 22.04/24.04 PC 与 Jetson 系统。ARM64 使用原生 AArch64 构建。当前不支持从 x86 交叉编译，因为 protobuf 和 gRPC 代码生成除了目标库还需要可在构建主机执行的 `protoc` 与 `grpc_cpp_plugin`。

Verify the host architecture before configuring:

```sh
uname -m
```

The expected result is `aarch64` or `arm64`. The presets do not force a target architecture; the native compiler selects the host target.

## Prerequisites

Install CMake 3.22 or newer, a C++17 compiler, Boost 1.74 or newer, spdlog, Git, `protoc`, and Python 3 with `protobuf==3.20.3`. Tests additionally require GoogleTest. On Ubuntu 22.04 and Jetson installations based on it:

```sh
sudo apt update
sudo apt install build-essential cmake git libboost-dev libgtest-dev libspdlog-dev python3
```

CMake uses compatible system copies of protobuf, gRPC, toml++, nlohmann/json, and nlohmann/json-schema-validator when available. Otherwise it fetches pinned sources while configuring, which requires network access to the repositories declared in `adapters/eos-cxx/CMakeLists.txt`. The fetched gRPC tree is excluded from the default build except for targets required by EOS, avoiding compilation of unrelated examples and optional libraries on edge systems.

For the shortest clean build on Ubuntu, install the available ARM64 protobuf and gRPC development packages before configuring:

```sh
sudo apt install libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc
```

System packages avoid compiling protobuf and gRPC locally. If their versions are incompatible with EOS, remove the ARM64 build tree and configure again without them so CMake uses the pinned fallback.

Do not copy an x86 `build/` directory to ARM64. Native `*.skill.so` libraries and all of their linked native dependencies must also be rebuilt for ARM64; an x86 skill library cannot be loaded into an ARM64 `eosd` process.

Create `.venv` with `uv sync --locked --group contracts`, or run `python3 -m venv .venv` and `.venv/bin/pip install protobuf==3.20.3`. The build shortcut selects this environment automatically. The contract generators support Ubuntu 22.04's `protoc` 3.12.

## Build and test

Configure, build, and test the debug ARM64 tree from the repository root:

```sh
cmake --preset arm64-dev -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build --preset arm64-dev -j
ctest --preset arm64-dev
```

For a release-oriented build without tests:

```sh
./build.sh
```

The shortcut prebuilds fetched gRPC/protobuf and JSON Schema Validator targets with conservative parallelism before compiling EOS. It also derives the main job count from CPU and available memory to avoid swap-heavy builds. Set `AETHER_BUILD_JOBS` or `AETHER_DEPENDENCY_JOBS` to a positive integer when the host needs an explicit limit.

Fresh trees use Ninja automatically when it is installed. To discard stale or cross-platform artifacts and rebuild the selected host architecture from scratch, run `./build.sh --clean`. The command removes only the selected architecture's core build tree.

Both ARM64 presets write to `build/aether-arm64`. If that directory was configured on another architecture, remove it before configuring on ARM64.

## Run

公开启动脚本运行 Rust 运行时与客户端。通过 IsOS 启动已应用部署，再用本次运行的控制套接字连接 EOS 客户端：

```sh
./bin/eos-cli --socket /absolute/path/to/eos-control.sock
```

`./bin/eos-repl` 是该 Rust 客户端的兼容名称，可用命令包括 `discover`、`health <path>`、`stop <path>` 和 `quit`，参见[客户端指南](repl_cn.md)。若启用 C++ 侧链、north HTTP 与 REPL 选项，另行构建的 C++ HTTP/SSE 客户端为 `build/aether-arm64/adapters/eos-cxx/ext/repl/eos-repl`，其命令契约不同。ROS 2 仅在绑定选择 ROS 2 设备时需要。source checkout 的 state 与分进程 log 保持在 `var/`。

## ROS 2 on ARM64

ROS 2 是可选后端，只有绑定选择它时才需要。Humble 与 Jazzy 分别支持对应的 Ubuntu 发行版。EOS 核心使用相同的 ARM64 构建流程；桥接包需在加载匹配的 ROS 2 环境和机器人 workspace 后单独构建：

```sh
source /opt/ros/humble/setup.bash
source /absolute/path/to/ros2-workspace/install/setup.bash
./bin/island-pkg build /path/to/private-ros2-backend --cmake-source .
```

Run that binary with an absolute deployment configuration that selects a ROS 2 backend binding:

```sh
./bin/eosd --config /absolute/path/to/eos.toml
```

构建桥接包不会更改 `etc/eos/eos.toml` 中选择的 profile；需显式更新 `[[mode.binding]]`。

## Troubleshooting

- `Could not read presets`: verify `cmake --version` reports 3.22 or newer.
- `Could NOT find Boost` or `spdlog`: install the prerequisite development packages for ARM64.
- `grpc_cpp_plugin.exe` on Linux/WSL: remove the build directory and reconfigure after removing Windows Anaconda entries from `PATH`; EOS accepts a native gRPC CMake package or Ubuntu's `protobuf-compiler-grpc` pkg-config installation, never a Windows generator.
- Dependency clone failures: provide network access to the FetchContent repositories or install compatible system packages.
- `native executable not found`: build the ARM64 preset; for a nonstandard build tree, set `ISLAND_BUILD_DIR` to its absolute path.
- Skill load errors such as `wrong ELF class` or `Exec format error`: rebuild the skill and every native library it uses for ARM64.
- Address already in use: 分别修改 EOS `--grpc-port` 与 adapter `--port`，并让 adapter `--upstream` 指向新的 gRPC endpoint。

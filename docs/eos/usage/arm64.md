# ARM64 running guide

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Usage](README.md) / **arm64**

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [usage/](README.md)
      - **arm64.md**
      - [backends.md](backends.md)
      - [configuration-reference.md](configuration-reference.md)
      - [configure.md](configure.md)
      - [mujoco.md](mujoco.md)
      - [packages.md](packages.md)
      - [repl.md](repl.md)

</details>
<!-- docs-tree:end -->

This page describes the opt-in C++ side chain on ARM64. The default EOS v2 runtime is Rust; use the [v2 build and launch guide](../develop/build-and-run.md) for its build and IsOS deployment path. The C++ side chain has no x86-only source or compiler flags. ARM64 support here is a native Linux build path: compile this side chain and every native dependency on the ARM64 target. The `x86_64` and `arm64` presets use separate `build/aether` and `build/aether-arm64` trees so objects and CMake caches are not shared.

## Supported path

The current platform baseline is Ubuntu 22.04 or 24.04 on NVIDIA GPU PCs and Jetson systems. ARM64 uses a native AArch64 build. Cross-compiling from x86 is not currently supported because protobuf and gRPC generation requires host-executable `protoc` and `grpc_cpp_plugin` tools in addition to target libraries.

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

Fresh trees use Ninja automatically when it is installed. To discard stale or cross-platform artifacts and rebuild the core from scratch, run `./build.sh --clean`. The command removes only the host architecture's core build tree.

Both ARM64 presets write to `build/aether-arm64`. If that directory was configured on another architecture, remove it before configuring on ARM64.

## Run

The public launchers start the Rust runtime and client. Launch the applied deployment through IsOS, then use the EOS client with that run's control socket:

```sh
./bin/eos-cli --socket /absolute/path/to/eos-control.sock
```

`./bin/eos-repl` is a compatibility name for that Rust client. Its commands include `discover`, `health <path>`, `stop <path>` and `quit`; see the [client guide](repl.md). The separately built C++ HTTP/SSE client is `build/aether-arm64/adapters/eos-cxx/ext/repl/eos-repl` when the C++ side chain, north HTTP and REPL options are enabled. It has a different command contract. ROS 2 is required only for bindings that select ROS 2 devices. Runtime state and per-service logs from a source checkout remain below `var/`.

## ROS 2 on ARM64

ROS 2 is optional and required only for bindings that select its backend. Humble and Jazzy are supported on their matching Ubuntu releases. Build EOS core with `./build.sh` without sourcing ROS. To build the separate bridge package, source the matching ARM64 ROS 2 installation and robot workspace:

```sh
source /opt/ros/humble/setup.bash
source /absolute/path/to/ros2-workspace/install/setup.bash
./bin/island-pkg build /path/to/private-ros2-backend --cmake-source .
./bin/island-pkg activate private/ros2-bridge
./bin/island-pkg lock add private/ros2-bridge
```

Review the printed schema v3 module, include its `module.toml` path in the runtime stack's `modules` list, and add any required driver declarations before starting the managed graph:

```sh
./launch.sh
```

Package activation supplies the bridge process. It does not change the hardware binding; update `[[mode.binding]]` in the selected EOS configuration explicitly.

## Troubleshooting

- `Could not read presets`: verify `cmake --version` reports 3.22 or newer.
- `Could NOT find Boost` or `spdlog`: install the prerequisite development packages for ARM64.
- `grpc_cpp_plugin.exe` on Linux/WSL: remove the build directory and reconfigure after removing Windows Anaconda entries from `PATH`; EOS accepts a native gRPC CMake package or Ubuntu's `protobuf-compiler-grpc` pkg-config installation, never a Windows generator.
- Dependency clone failures: provide network access to the FetchContent repositories or install compatible system packages.
- `native executable not found`: build the ARM64 preset; for a nonstandard build tree, set `ISLAND_BUILD_DIR` to its absolute path.
- Skill load errors such as `wrong ELF class` or `Exec format error`: rebuild the skill and every native library it uses for ARM64.
- Address already in use: choose a different `eosd --grpc-port`, then set the adapter `--upstream` to it; choose adapter `--port` independently and pass that HTTP port to the REPL.

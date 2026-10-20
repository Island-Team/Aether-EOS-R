# ARM64 running guide

EOS is portable C++17 and has no x86-only source or compiler flags. ARM64 support is a native Linux build path: compile EOS and every native dependency on the ARM64 target. The existing `default` and `dev` presets remain the primary x86 workflow and continue to use `build/aether`; ARM64 is opt-in and uses `build/aether-arm64` so objects and CMake caches are never shared between architectures.

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

CMake uses compatible system copies of protobuf, gRPC, toml++, nlohmann/json, and nlohmann/json-schema-validator when available. Otherwise it fetches pinned sources while configuring, which requires network access to the repositories declared in `src/eos/CMakeLists.txt`. The fetched gRPC tree is excluded from the default build except for targets required by EOS, avoiding compilation of unrelated examples and optional libraries on edge systems.

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
./eos-build.sh
```

The shortcut prebuilds fetched gRPC/protobuf and JSON Schema Validator targets with conservative parallelism before compiling EOS. It also derives the main job count from CPU and available memory to avoid swap-heavy builds. Set `AETHER_BUILD_JOBS` or `AETHER_DEPENDENCY_JOBS` to a positive integer when the host needs an explicit limit.

Fresh trees use Ninja automatically when it is installed. To discard stale or cross-platform artifacts and rebuild the selected host architecture from scratch, run `./eos-build.sh --clean`. The command removes only the build tree selected for the current architecture, backend, and ROS 2 distribution.

Both ARM64 presets write to `build/aether-arm64`. If that directory was configured on another architecture, remove it before configuring on ARM64.

## Run

The public source launchers detect the configured backend, ROS distribution, and ARM64 host to select the matching build tree automatically:

```sh
./bin/eosd
```

In other terminals, start north HTTP and connect the diagnostic client:

```sh
./bin/eos-north-http
./bin/eos-repl
```

The bundled configuration selects the hardware-free `demo` profile and needs no ROS 2 environment. Source a compatible ROS 2 distribution and workspace when a deployment-owned binding selects ROS 2 devices. The gateway listens on gRPC at `127.0.0.1:16003`; the explicitly launched `eos-simulator` uses loopback port `16004` and `<run>/simulator.sock`; and north HTTP provides HTTP/SSE at `127.0.0.1:16005`. `eosd` does not start or supervise it or north HTTP. Stop any foreground process with Ctrl-C; simulator, runtime, north HTTP, and REPL have independent production lifecycles. `eos-launch.sh` supervises the complete stack for development.

For a non-interactive smoke check, start simulator, `eosd`, and north HTTP, then pipe `status` and `quit` to `eos-repl`. Runtime state and per-service logs from a source checkout remain below `var/`.

## ROS 2 on ARM64

ROS 2 is optional and required only for bindings that select its backend. Humble and Jazzy are supported on their matching Ubuntu releases. Source the matching ARM64 ROS 2 installation and robot workspace; `eos-build.sh` detects the ROS 2 profile and uses `ROS_DISTRO` to select a dedicated preset and build directory:

```sh
source /opt/ros/humble/setup.bash
source /absolute/path/to/ros2-workspace/install/setup.bash
./eos-build.sh
```

Run that binary with an absolute deployment configuration that selects a ROS 2 backend binding:

```sh
ISLAND_BUILD_DIR="$PWD/build/aether-arm64-ros2-humble" \
  ./bin/eosd --config /absolute/path/to/eos.toml
```

Enabling `EOS_BACKEND_ROS2` only compiles the backend. It does not change the profile selected in `etc/eos/eos.toml`; update `[[mode.binding]]` explicitly.

## Troubleshooting

- `Could not read presets`: verify `cmake --version` reports 3.22 or newer.
- `Could NOT find Boost` or `spdlog`: install the prerequisite development packages for ARM64.
- `grpc_cpp_plugin.exe` on Linux/WSL: remove the build directory and reconfigure after removing Windows Anaconda entries from `PATH`; EOS accepts a native gRPC CMake package or Ubuntu's `protobuf-compiler-grpc` pkg-config installation, never a Windows generator.
- Dependency clone failures: provide network access to the FetchContent repositories or install compatible system packages.
- `native executable not found`: build the ARM64 preset; for a nonstandard build tree, set `ISLAND_BUILD_DIR` to its absolute path.
- Skill load errors such as `wrong ELF class` or `Exec format error`: rebuild the skill and every native library it uses for ARM64.
- Address already in use: choose a different `eosd --grpc-port`, then set the adapter `--upstream` to it; choose adapter `--port` independently and pass that HTTP port to the REPL.

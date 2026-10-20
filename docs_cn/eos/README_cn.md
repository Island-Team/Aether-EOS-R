# EOS (Embodied OS) in IslandOS-Aether

EOS 是 Island 团队 Aether 产品版本中的执行与硬安全组件，负责设备集成、只读机体投影、可选感知模型客户端、命令准入及硬件释放边界。EGIA 仍是独立进程，通过 `island.eos.v1.GatewayService` 等服务和 `share/contracts/eos/` 下的公开契约与 EOS 通信。

EOS 历史导入合并产品树的 `src/eos/` 下。`share/contracts/eos/` 下的专注 proto 文件是维护的 Agent↔EOS 接口源码；生成的运行时 JSON 位于 `share/ext/eos/`。产品布局和开发约定见 `docs/island/`。

## Current implementation

- `src/eos/runtime/init`: strict process config, immutable schemas, and startup composition.
- `src/eos/runtime/hardware`: backend ownership, one-binding admission, and process-scoped identity.
- `src/eos/runtime/perception`: read-only semantic projection；当前由 `perception/body` 实现 body、world、vision、speech 与 status resource。
- `src/eos/runtime/models`: 规范 model schema、configured registry、bounded worker、HTTP/WebSocket transport 与 normalized health；provider conversion 位于 `src/eos/ext/models`。
- `src/eos/runtime/dispatch`: schema-validated action gate, limits, leases, stop/estop, and backend release.
- `src/eos/runtime/skills`: 共享执行生命周期与日志、已实现的 native profile，以及 fail-closed 的 managed/external 占位模块。
- `src/eos/ext/decision`: backend-neutral northbound decision client、policy interface 与 reconnecting event runner。
- `src/eos/runtime/northbound`: gRPC-only Gateway、Skill 与 Workflow transport。
- `src/eos/ext/north_http`: REPL/WebUI 使用的独立 REST/SSE-to-gRPC 进程，包含 focused config、一个 shared EOS gRPC client、server module 与可选 external non-authoritative ActionList audit journal；参见 [module 总览](develop/ext/north_http/README_cn.md)。
- `src/eos/ext/backends`: dependency-free demo adapters、部署侧专有适配器、原生 WAV-to-ALSA `aplay` speaker，以及可选 ROS 2 Humble/Jazzy arm/gripper/camera bridge.
- `src/eos/ext/repl`: standalone contract-level diagnostic client.

Public EOS developer headers live under `src/eos/include/eos/`. This includes the hardware adapter API under `hardware/` and the native skill ABI under `skill/`; runtime-internal interfaces stay beside their implementations.

## EOS setup guide

This setup runs EOS only. It does not start an Agent, model server, ROS graph, or vendor daemon for you.

### 1. Build EOS

From the Island checkout root:

```sh
cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON
cmake --build build/aether -j
ctest --test-dir build/aether/src/eos --output-on-failure
```

For ARM64 use the [ARM64 running guide](usage/arm64_cn.md). For ROS 2 hardware, source ROS 2 and the workspace containing your graph-edge adapter first, then configure with `-DEOS_BACKEND_ROS2=ON`; see the [southbound interface](develop/interfaces/southbound/driver_cn.md#ros-2-arm-mapping).

### 2. Choose or create a hardware profile

EOS binds one complete hardware profile per process. The committed default is the public hardware-free demo profile:

```toml
[mode]
[[mode.binding]]
backend = "backends/demo.toml"
profile = "demo"
```

Use `backends/demo.toml` / `demo` as the canonical public example for a true-hardware profile. For custom hardware, select or create a deployment-owned binding, then point `[[mode.binding]]` at it. The binding declares the robot, devices, supported primitives, feed-rate envelopes, and transport. EOS uses the profile's stable `robot_id` and `robot_name` and generates device IDs; do not configure device IDs. The complete walkthrough is in [配置自有硬件](../../README_cn.md#配置自有硬件), and the full key reference is in [Configuration](usage/configuration_cn.md#binding-files).

The selection logic is deliberately one-way: `eos.toml` selects one profile; the profile declares semantic devices and chooses a backend per device; the backend forwards standard EOS commands and telemetry to a deployment-owned driver, ROS graph, or system ALSA route. A site endpoint, joint name, limit, frame, audio route, or calibration belongs in the profile or graph-edge adapter, never in reusable EOS runtime code.

Available integration paths are:

- For a ROS 2 arm, gripper, or camera, use `backend.type = "ros2"`, configure its standard `JointState`, `PoseStamped`, `CompressedImage`, `Float64MultiArray`, and `Empty` topics as applicable, and bridge any vendor protocol in your own ROS graph. See [ROS 2 arm mapping](develop/interfaces/southbound/driver_cn.md#ros-2-arm-mapping).
- For a system speaker, use `backend.type = "aplay"` with `[device.backend.aplay]` playback settings such as `alsa_device = "default"`. Bind `speaker.speak` to the exact TTS model with its `[[device.primitive]] model_id`; `speaker.play` needs no model. `eosd` validates the bounded WAV asset and directly invokes `aplay -q -D DEVICE` without a shell. See [Aplay 扬声器后端](develop/ext/backends/host_cn.md#aplay-扬声器后端).
- For another native device or transport, implement `eos::hardware::Backend` under `src/eos/ext/backends/` and add its CMake target. Put class-specific part parsing, validation, and backend construction in the matching `runtime/hardware/<class>.cpp`, route the class from `runtime/hardware/common.cpp`, and extend common backend-name admission in `runtime/hardware/binding.cpp` when introducing a new transport type. The shared `PrimitiveSpec` catalog admits binding declarations and is consumed by Dispatcher for discovery and release; execution policy remains in dispatch. The required methods, command units, safety behavior, and checklist are in the [southbound driver contract](develop/interfaces/southbound/driver_cn.md).

Keep physical geometry and authoritative limits in the hardware authority (for example URDF or the vendor controller), and mirror caller-visible numeric intervals into each bounded `[[device.primitive]].limits` declaration. EOS validates those ranges during registry admission, advertises them to agents, and enforces them before release. Northbound commands use explicit metres, radians, seconds, and newtons; the backend must still reject values outside the real device envelope.

### 3. Link custom perception models

Models are outbound HTTP or WebSocket services. EOS does not download weights or start inference servers. Start your service separately, then add one `[[perception.model]]` entry to the selected `eos.toml`:

```toml
[[perception.model]]
id = "site-human-pose"
enabled = true
capability = "vision.human_pose"
transport = "websocket"
endpoint = "ws://127.0.0.1:18014/v1/stream/pose"
wire_format = "json"
timeout_ms = 3000
# api_key_env = "SITE_MODEL_TOKEN"
# provider_model_id = "deployment-model-name"
# max_request_bytes = 16777216
# max_response_bytes = 16777216
```

支持的 capability 包括 `audio.vad`、`audio.transcription`、`audio.synthesis`、`vision.object_detection`、`vision.human`、`vision.human_pose` 与 `vision.face`。Capability request/response schema 是 `share/contracts/eos/external/models/schemas/` 下的私有 runtime interface；model 不是 Agent-facing gateway API。HTTP 是默认 transport；持久 camera vision client 使用 `transport = "websocket"`。参见 [Configuration](usage/configuration_cn.md) 与 [Perception model clients](design/runtime/models_cn.md)。

`enabled` 可选，默认为 `true`，且只在启动时读取。禁用条目仍完整校验并参与 ID 去重，但不可调用。EOS 没有 local fallback 或按 capability 自动替换。连续 VAD 与 camera processing 另由 `[perception.stream]` 控制；四个开关都默认 `false`，已提交配置启用 `vad`、`human` 与 `human_pose`，禁用 `face`。启用的 stream 和 skill one-shot inference 使用已配置的 role binding；模型缺失、禁用或 capability 错误时失败。模型条目与 provider adapter 没有运行时热重载，变更后需要重启 EOS。

### 4. Launch and verify

Demo profile 先启动 simulator，再启动 EOS：

```sh
./bin/eos-simulator
```

```sh
EOS_ETC=/absolute/path/to/etc/eos \
EOS_CONTRACTS=/absolute/path/to/share \
./bin/eosd
```

In another terminal:

```sh
./bin/eos-north-http
```

```sh
./bin/eos-repl
```

```sh
curl http://127.0.0.1:16005/v1/eos/discover
curl http://127.0.0.1:16005/v1/eos/resources
```

EOS core 只提供 protobuf **gRPC services**；独立 north HTTP 为 Gateway、Skill、 Workflow unary 与 stream 提供严格 **HTTP/ProtoJSON + SSE**。完整 WebUI 契约见 [north HTTP](develop/interfaces/external/http_cn.md)，另见 [northbound wire](develop/interfaces/grpc_cn.md)、 [primitive payload](develop/interfaces/actions-payloads_cn.md) 与 [contract inventory](develop/interfaces/contracts_cn.md)。

```sh
./scripts/print_contract_interfaces.py --contracts /absolute/path/to/share
```

## Build and test

Configure from the combined Island checkout root. The product CMake project discovers `src/eos/CMakeLists.txt` and adds the native component.

```sh
cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON
cmake --build build/aether -j
ctest --test-dir build/aether/src/eos --output-on-failure
```

EOS requires CMake 3.22 or newer, a C++17 compiler, Boost 1.74 or newer, spdlog, toml++, nlohmann/json 3.11 or newer, and nlohmann/json-schema-validator. A deployment selecting the `aplay` backend also requires `alsa-utils`. CMake uses system packages when available and otherwise fetches the pinned portable dependencies into the selected build tree.

The standard presets remain the primary x86 workflow. For a native ARM64 build and its separate build tree, follow the [ARM64 running guide](usage/arm64_cn.md).

`./eos-build.sh` is the backend- and architecture-detecting release shortcut. It reads the profile selected by `etc/eos/eos.toml`. Non-ROS profiles use the `default` preset and `build/aether` on x86_64/amd64, or the `arm64` preset and `build/aether-arm64` on aarch64/arm64. ROS 2 profiles use the sourced `ROS_DISTRO` to select an isolated Humble or Jazzy tree. The script builds fetched gRPC/protobuf and other compiled dependencies first with a lower parallelism cap, preventing the largest translation units from competing with the EOS build for memory. The main job count adapts to available memory and CPU count. Override either limit with `AETHER_BUILD_JOBS` or `AETHER_DEPENDENCY_JOBS`. CMake also uses `ccache` automatically when available, including for fetched dependencies. Fresh shortcut builds use Ninja when available; `./eos-build.sh --clean` removes and rebuilds only the selected isolated tree.

For ROS 2:

```sh
source /opt/ros/humble/setup.bash
source /path/to/ros2-workspace/install/setup.bash
./eos-build.sh
```

## Launch

The public wrappers resolve their own location, so they work from any current directory:

```sh
./bin/eos-simulator
# In another terminal:
./bin/eosd
# In another terminal:
./bin/eos-north-http
# In another terminal:
./bin/eos-repl
```

`./eos-launch.sh` 用于开发监管 simulator → EOS → north HTTP → REPL，等待每个 readiness boundary，并在失败或 signal 时逆序关闭。`--no-simulator` 与 `--no-repl` 可省略对应进程；north HTTP/repl override 见 `--help`。

In a source checkout, `bin/eosd`、`bin/eos-north-http`、`bin/eos-simulator`、 `bin/eos-repl` 和 `eos-launch.sh` 在 x86_64 使用 `build/aether`，在 ARM64 使用 `build/aether-arm64`，需要 ROS 时再附加对应 suffix。设置绝对路径 `ISLAND_BUILD_DIR` 可让它们统一使用其他 build tree。每个 wrapper 会在启动私有 native executable 前解析并导出 Island filesystem roles。

The runtime accepts the existing overrides:

- `--config PATH`: EOS process configuration; defaults to `etc/eos/eos.toml`.
- `--etc PATH` or `EOS_ETC`: EOS configuration root.
- `--contracts PATH` or `EOS_CONTRACTS`: canonical Agent-runtime contract root.
- `--skills-dir PATH` or `EOS_SKILLS`: optional absolute directory of deployment-owned `*.skill.so` libraries.
- `--host`, `--grpc-port`, and `--log-level`: EOS runtime behavior.

north HTTP、REPL 与 simulator 都是显式外部进程；生产环境分别管理。`eos-repl` 只连接 north HTTP，不直连 EOS。HTTP listener/upstream/CORS/limit 由严格 `north-http.toml` 管理。

On an interactive terminal, `eos-repl` provides editable persistent history, Ctrl-R search, hints, and runtime-aware Tab completion. Primitive、skill 与 signal argument 接受 `key=value` 或 `--json`，workflow 接受 `--json`；四者都支持带 local validation/preview 的 `--prompt`; see the [diagnostic REPL reference](develop/ext/tools/repl_cn.md) for keys, history location, JSON editing, and client-only options.

每个 `eosd` 对应一个 hardware profile 与一个 gRPC endpoint；需要 HTTP 时配套 独立 north HTTP listener。另一台 robot 使用不同 endpoint。REPL 连接 north HTTP 后 自动打印 `Discover`。

Product-wide roots use `ISLAND_ETC`, `ISLAND_SHARE`, `ISLAND_STATE`, `ISLAND_CACHE`, `ISLAND_LOG`, `ISLAND_RUN`, and `ISLAND_TMP`. Caller-supplied roots must be absolute. `EOS_VAR` remains a compatibility root for mutable roles. Normal checkout writes are confined to `var/`.

Stop a foreground runtime with SIGINT or SIGTERM. Installed processes are stopped by their service manager; the EOS import does not provide a process-name or port-based kill helper.

## Development

Run a test-enabled build before changing EOS, and keep the implementation at the correct boundary:

- Public, backend-neutral device operations belong in `src/eos/include/eos/hardware/backend.h`.
- Profile parsing, backend admission, and factories belong in `src/eos/runtime/hardware/`.
- Concrete adapters belong in `src/eos/ext/backends/` and consume all deployment alignment from the selected profile.
- Physical hardware tests and site configuration remain deployment-owned; repository unit tests use fakes, loopback transports, or simulation.

Use an existing backend whenever configuration can express the integration. When a new transport really requires code, follow the ordered checklist under [Add a backend type](../../README.md#add-a-backend-type), add parser and registry coverage, and document every new public key or interface. Run `ctest --preset dev` before handoff.

## Installed mapping

`eosd`、`eos-north-http`、`eos-simulator`、`eos-repl` public wrapper 安装到 `/usr/bin`，native executable 安装到 `/usr/libexec/island`；配置位于 `/etc/island/eos`，contract 位于 `/usr/share/island/contracts`。

## Documentation

- [Documentation index](doc_index_cn.md)
- [ARM64 running guide](usage/arm64_cn.md)
- [Island repository layout](../island/layout_cn.md)
- [Coding conventions](../island/conventions_cn.md)
- [Technical solution](design/eos-2607_cn.md) ([Chinese](design/eos-2607_cn.md))
- [Algorithm and edge optimization notes](design/algorithms_cn.md)
- [Northbound interface](develop/interfaces/grpc_cn.md)
- [north HTTP 与 WebUI 契约](develop/interfaces/external/http_cn.md)
- [EOS 调试 runbook](usage/debugging_cn.md)
- [Southbound interface](develop/interfaces/southbound/driver_cn.md)
- [Configuration](usage/configuration_cn.md)
- [Canonical contracts](develop/interfaces/contracts_cn.md)
- [Runtime initialization](design/runtime/init_cn.md)
- [Gateway](design/runtime/northbound_cn.md)
- [Body perception](design/runtime/body_cn.md)
- [Perception model clients](design/runtime/models_cn.md)
- [Bridge](design/runtime/hardware_cn.md)
- [Dispatch](design/runtime/dispatch_cn.md)
- [Write native skills](develop/interfaces/skills/native/README_cn.md)
- [Native skill runtime](design/runtime/skills_cn.md)

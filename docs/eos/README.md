# EOS (Embodied OS) in IslandOS-Aether

EOS, the Embodied OS, is the native execution and hard-safety component of Aether, an Island product version. It owns device integration, read-only body projection, a general models runtime, command admission, and the controlled release boundary before hardware. EGIA remains a separate process and communicates with EOS through `island.eos.v1.GatewayService`, the dedicated `island.eos.v1.SkillService`, and the public contract source under `share/contracts/eos/`.

The EOS history is imported into the combined product tree under `src/eos/`. Focused proto files under `share/contracts/eos/` are the maintained Agent↔EOS interface source; generated runtime JSON lives under `share/ext/eos/`. Product layout and development conventions live under `docs/island/`.

## Current implementation

- `src/eos/runtime/init`: strict process config, immutable schemas, and startup composition.
- `src/eos/runtime/hardware`: backend ownership, one-binding admission, and process-scoped identity.
- `src/eos/runtime/perception`: read-only semantic projection, currently implemented by `perception/body` for body, world, vision, speech, and status resources.
- `src/eos/runtime/models`: canonical model schemas, the configured registry, bounded workers, generic HTTP transport, and normalized health; provider conversion lives in `src/eos/ext/models`.
- `src/eos/runtime/dispatch`: schema-validated action gate, limits, leases, stop/estop, and backend release.
- `src/eos/runtime/skills`: SkillManager catalog, claim-aware scheduling, SIGNAL/events, native ABI v2, and fail-closed managed/external provider placeholders.
- `src/eos/ext/decision`: backend-neutral northbound decision client, policy interface, and reconnecting event runner.
- `src/eos/runtime/northbound`: gRPC-only Gateway, Skill, and Workflow transport.
- `src/eos/ext/north_http`: standalone REST/SSE-to-gRPC process with focused config, one shared EOS gRPC client, server modules for REPL/WebUI clients, and an optional external non-authoritative ActionList audit journal; see the [module overview](develop/ext/north_http/README.md).
- `src/eos/ext/backends`: dependency-free demo adapters, the native WAV-to-ALSA `aplay` speaker, optional ROS 2 Humble/Jazzy arm/gripper/camera bridge, and deployment-specific adapters.
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

For ARM64 use the [ARM64 running guide](usage/arm64.md). For ROS 2 hardware, source ROS 2 and the workspace containing your graph-edge adapter first, then configure with `-DEOS_BACKEND_ROS2=ON`; see the [southbound body interface](develop/interfaces/southbound/body.md).

### 2. Choose or create a hardware profile

EOS binds one complete hardware profile per process. The committed default is the public hardware-free demo profile:

```toml
[mode]
[[mode.binding]]
backend = "backends/demo.toml"
profile = "demo"
```

Use `backends/demo.toml` / `demo` as the canonical public example for a true-hardware profile. For custom hardware, select or create a deployment-owned binding, then point `[[mode.binding]]` at it. The binding declares the robot, devices, supported primitives, feed-rate envelopes, and transport. The profile supplies stable `robot_id` and `robot_name` values (with UUIDv7 only as the `robot_id` fallback), while EOS generates deterministic device IDs; do not configure device IDs. The complete walkthrough is in [Configure a robot](../../README.md#configure-a-robot), and the full key reference is in [Configuration](usage/configuration.md#binding-files).

The selection logic is deliberately one-way: `eos.toml` selects one profile; the profile declares semantic devices and chooses a backend per device; the backend forwards standard EOS commands and telemetry to a deployment-owned driver, ROS graph, or system ALSA route. A site endpoint, joint name, limit, frame, audio route, or calibration belongs in the profile or graph-edge adapter, never in reusable EOS runtime code.

Available integration paths are:

- For a ROS 2 arm, gripper, or camera, use `backend.type = "ros2"`, configure its standard `JointState`, `PoseStamped`, `CompressedImage`, `Float64MultiArray`, and `Empty` topics as applicable, and bridge any vendor protocol in your own ROS graph. See the [ROS 2 backend](develop/ext/backends/ros2.md).
- For a system speaker, use `backend.type = "aplay"` with `[device.backend.aplay] alsa_device = "default"` (or a verified deployment route) and an optional `max_stream_bytes`. EOS resolves and validates one bounded audio asset for `speaker.play@1.0.0`, then invokes `aplay -q -D DEVICE` without a shell. The advanced `speaker.speak` primitive reports the health of its TTS-to-speaker chain. See [Aplay speaker backend](develop/ext/backends/host.md#aplay-speaker-backend).
- For another native device or transport, implement `eos::hardware::Backend` and a backend descriptor under `src/eos/ext/backends/`, then register it in the built-in catalog. The descriptor owns settings parsing, supported classes and primitives, validation, and construction. Keep shared part geometry and primitive admission in `runtime/hardware/`. The shared `PrimitiveSpec` catalog is consumed by Dispatcher for discovery and release; execution policy remains in dispatch. The required receive boundary is in [southbound body](develop/interfaces/southbound/body.md), and command behavior is in [southbound primitives](develop/interfaces/southbound/primitives.md).

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

Choose one supported capability such as `audio.vad`, `audio.transcription`, `audio.synthesis`, `vision.object_detection`, `vision.human`, `vision.human_pose`, or `vision.face`. Capability request/response schemas are private runtime interfaces under `share/contracts/eos/external/models/schemas/`; models are not Agent-facing gateway APIs. HTTP is the default transport; persistent camera vision clients use `transport = "websocket"`. A provider may expose an optional HTTP health endpoint. See [Configuration](usage/configuration.md) and the [models runtime](design/runtime/models.md).

`enabled` is optional, defaults to `true`, and is read only at startup; restart EOS after changing it. Disabled entries remain fully validated and participate in ID deduplication, but are absent from discovery and return `NOT_FOUND` when called. EOS does not substitute built-in inference. Continuous VAD and camera processing is controlled separately by `[perception.stream]`; all four switches default to `false`, while the committed configuration enables only `vad` and `human`. Enabled streams and one-shot skill inference use the configured role bindings and fail on missing, disabled, or wrong-capability models.

### 4. Launch and verify

For the demo profile, start the simulator first. Then point EOS at absolute deployment roots when they differ from the checkout defaults:

```sh
./bin/eos-simulator
```

```sh
EOS_ETC=/absolute/path/to/etc/eos \
EOS_CONTRACTS=/absolute/path/to/share \
./bin/eosd
```

Start north HTTP and the client in other terminals:

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

The interface has one contract stack: protobuf **gRPC services** in EOS, canonical **JSON payloads** in dynamic fields, and a standalone **HTTP/ProtoJSON + SSE service** covering Gateway, Skill, and Workflow. Read the [north HTTP/WebUI contract](develop/interfaces/external/http.md), northbound [body](develop/interfaces/body.md), [world](develop/interfaces/world.md), [primitive](develop/interfaces/actions.md), and [skill](develop/interfaces/skills.md) interfaces, or render the complete surface with:

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

The standard presets remain the primary x86 workflow. For a native ARM64 build and its separate build tree, follow the [ARM64 running guide](usage/arm64.md).

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

`./eos-launch.sh` combines this development workflow in one terminal. It starts the simulator by default, then EOS, north HTTP, and the REPL; waits for TCP, gRPC-backed north HTTP readiness, and tears all children down in reverse order on exit or a signal. `--no-simulator` and `--no-repl` omit those optional processes. `--north-http-config`, `--north-http-host`, `--north-http-port`, and repeated `--repl-arg` values configure the client edge; other runtime arguments are forwarded to `eosd`.

In a source checkout, the `eosd`, `eos-north-http`, `eos-simulator`, and `eos-repl` wrappers plus `eos-launch.sh` select the architecture/profile build tree. Set an absolute `ISLAND_BUILD_DIR` to select another tree. Every wrapper resolves and exports the Island filesystem roles before starting its private native executable and works from an arbitrary current directory.

The runtime accepts the existing overrides:

- `--config PATH`: EOS process configuration; defaults to `etc/eos/eos.toml`.
- `--etc PATH` or `EOS_ETC`: EOS configuration root.
- `--contracts PATH` or `EOS_CONTRACTS`: canonical Agent-runtime contract root.
- `--skills-dir PATH` or `EOS_SKILLS`: optional absolute directory of deployment-owned `*.skill.so` libraries.
- `--host`, `--grpc-port`, and `--log-level`: EOS runtime behavior.

North HTTP listener/upstream/CORS/limit settings are owned by strict `north-http.toml`. `eos-repl` is an HTTP/SSE client and has no direct EOS connection. In production, all processes have separate service ownership; `eosd` never launches or monitors north HTTP, simulator, or REPL.

On the `nvidia` host, this checkout provides `etc/systemd/system/island-eosd.service` and `etc/systemd/system/island-eos-north-http.service`. Install them under `/etc/systemd/system/`, reload systemd, then launch the two services and the interactive client separately:

```sh
sudo install -m 0644 etc/systemd/system/island-eosd.service /etc/systemd/system/
sudo install -m 0644 etc/systemd/system/island-eos-north-http.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable island-eosd.service island-eos-north-http.service
sudo systemctl start island-eosd.service
sudo systemctl start island-eos-north-http.service
systemctl status island-eosd.service island-eos-north-http.service
./bin/eos-repl
```

The REPL connects to north HTTP on `127.0.0.1:16005`, which forwards to EOS on port `16003`. Both units use this checkout's configuration and build. Stop any other EOS stack using those ports or the same hardware before starting the units.

The host's preferred PulseAudio devices are the TAKSTAR MS-580USB microphone and Pebble V3 speaker. `sys_dev.sh --ensure-preferred` selects them when present; `--watch-preferred` retries every two seconds and restores those defaults after hotplug or an audio-server restart. Install the user service and keep the `nvidia` user manager running across logouts:

```sh
install -D -m 0644 etc/systemd/user/island-audio-defaults.service \
  ~/.config/systemd/user/island-audio-defaults.service
systemctl --user daemon-reload
systemctl --user enable --now island-audio-defaults.service
sudo loginctl enable-linger nvidia
./scripts/sys_dev.sh --current
```

A deployment binding should select stable ALSA routes. Microphone capture retries its configured card after a disconnect; speaker playback opens its configured route for each request.

On an interactive terminal, `eos-repl` provides editable persistent history, Ctrl-R search, hints, and runtime-aware Tab completion. Primitive, skill, and signal arguments accept `key=value` or `--json`; workflows accept `--json`. All four support guided `--prompt` entry with local validation and a final preview; see the [diagnostic REPL reference](develop/ext/tools/repl.md) for keys, history location, JSON editing, and client-only options.

Each `eosd` process represents one robot profile on one gRPC endpoint. Its north HTTP may use a separate port. Run another pair with distinct endpoints for another robot. The REPL prints `Discover` automatically after connecting and renders the discovered robot name in its prompt.

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

Public `eosd`, `eos-north-http`, `eos-simulator`, and `eos-repl` wrappers install under `/usr/bin`; their native executables install under `/usr/libexec/island`. The skill ABI header installs under `/usr/include/eos/skill`, configuration under `/etc/island/eos`, contracts under `/usr/share/island/contracts`, and documentation under `/usr/share/doc/island/eos`. Source, tests, build output, runtime skill libraries, and checkout state are not installed.

## Documentation

- [Documentation index](doc_index.md)
- [ARM64 running guide](usage/arm64.md)
- [Island repository layout](../island/layout.md)
- [Coding conventions](../island/conventions.md)
- [Technical solution](design/eos-2607.md) ([Chinese](../../docs_cn/eos/design/eos-2607_cn.md))
- [Algorithm and edge optimization notes](design/algorithms.md)
- [Northbound interfaces](develop/interfaces/README.md)
- [north HTTP and WebUI contract](develop/interfaces/external/http.md)
- [EOS debugging runbook](usage/debugging.md)
- [Southbound interfaces](develop/interfaces/README.md)
- [Configuration](usage/configuration.md)
- [Canonical contracts](develop/interfaces/contracts.md)
- [Runtime initialization](design/runtime/init.md)
- [Northbound runtime](design/runtime/northbound.md)
- [Perception runtime](design/runtime/perception.md)
- [Models runtime](design/runtime/models.md)
- [Hardware runtime](design/runtime/hardware.md)
- [Dispatch](design/runtime/dispatch.md)
- [Write native skills](develop/interfaces/skills/native/README.md)
- [Use SkillService and SkillManager](develop/interfaces/skills.md)
- [Skill runtime](design/runtime/skills.md)
- [External decision library](../../src/eos/ext/decision/README.md)

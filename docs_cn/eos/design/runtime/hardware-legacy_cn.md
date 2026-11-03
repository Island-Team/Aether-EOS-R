# Hardware runtime

[Runtime overview](README_cn.md) links the integrated runtime module set.

Runtime module at `src/eos/runtime/hardware/` — the sole owner of every south-bound backend instance. It parses a resolved binding, instantiates concrete adapters behind `eos/hardware/backend.h`, admits them at startup, forwards the bridge tick, and aggregates device health. It knows nothing north-bound: no JSON and no contract reads.

## What exists now

**Binding parsing** (`binding.cpp`, toml++). 每个 `[profile.<name>]` 包含 robot metadata 和 `[[device]]`。通用结构、part 几何信息及 EOS 自有 primitive catalog 位于 `runtime/hardware/`；`runtime/hardware/backends/` 或 `ext/backends/` 下的 descriptor 负责各后端的设置解析、支持的设备类型和 primitive、跨设备校验与构造。调用方显式传入 `BackendCatalog`。每个 `[[device.primitive]]` 都有精确 version 与 description，reference-aware motion 还声明允许的 `references`，absolute spatial target 声明 `absolute_frame`。Numeric limits 与 reference policy 在 startup admission 后通过 body discovery 发布，并由 Dispatcher 使用。Robot identity 通常来自 profile，device ID 按 binding 顺序确定为 `<class>-<index>`；未知键和不一致声明 fail closed。

**Mode resolution** (`mode.cpp`). Requires exactly one `[[mode.binding]]` from `etc/eos/eos.toml`. Additional robots use separate EOS gRPC endpoints and, when browser access is needed, separate adapter listeners. `[mode.primitives] expose_operator` defaults false.

**Identity and admission** (`identity.cpp`, `admission.cpp`). `Load` 一次性解析并接纳单个 hardware binding。配置的 `robot_id` 跨重启稳定；缺失时生成 process-lifetime UUIDv7。Registry 分配 `<class>-<index>` device ID，并调用所选 descriptor 的 factory。内置 catalog 在 `runtime/hardware/manager/backend_catalog.cpp` 组装，未编译的可选后端与未知类型分别报错。Admission 检查 feed rate 与 primitive policy。`aplay` factory 验证 ALSA route 与完整 asset byte bound；TTS 是 Dispatcher 中 advanced `speaker.speak` 的 dependency。

Demo adapter 是 EOS runtime 的必需组件。CMake 配置会拒绝 `EOS_BACKEND_DEMO=OFF`；可选 transport backend 仍由各自开关控制。

**Read-only feed seam.** `Device::identity()`, `feeds()`, part metadata, envelope, and health are const. Body perception and gateway can inspect those values but cannot command, clamp, halt, or tick a backend. Mutable backend access is private to bridge Registry and granted to dispatch's Dispatcher for controlled release. This makes bridge the only backend owner without creating a second actuator path.

**通用设备访问.** 调用方使用 `Registry::Find` 和通用 backend telemetry 方法；Registry 不暴露具体的 demo adapter 类型。

**Tick and health.** `Registry::Tick(dt_s)` 只转发给拥有本地工作的 backend。Simulator adapter 不推进 world，而是向外部 MuJoCo service 发起有界私有 RPC。模拟器缺失或断开时，后端 health 为 unavailable、命令被拒绝且 camera 不返回 frame，但 EOS 不会终止。Per-device health 是 `Backend::ReadHealth()` 返回的 backend-neutral `alive`/`admitted` 结果；只有显式 `safety.estop` primitive 设置 process-wide estop latch。

**Direct speaker path.** `aplay` descriptor 构造不依赖 model service 的硬件 backend。Dispatcher 为 `speaker.play` 解析 asset URI，或为 `speaker.speak` 获取合成结果，校验一个完整 16 kHz mono PCM WAV 后 release atomic `SpeakerCommand`。Backend 再次校验完整 asset，并直接执行 `aplay -q -D DEVICE`；它拥有 cancellation、child reaping 与 playback telemetry。详见 [Aplay 扬声器后端](../../develop/ext/backends/host_cn.md#aplay-扬声器后端)。

## Deliberately later

Hot-plug (device appear/disconnect handling), the inotify config watch with re-admission, and richer bridge-health/freshness admission in Dispatcher remain future work — see [支撑与侧向模块](../eos-2607_cn.md#支撑与侧向模块). Until then the registry is a startup-time construct: no runtime swapping, no reload.

## Testing

通用 `hardware_tests` 使用合成 binding 与 fake backend；后端的解析及行为测试位于各自模块。运行 `cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON && cmake --build build/aether -j && ctest --test-dir build/aether`。


## 后端管理器与软件包边界

后端管理器属于 EOS 运行时，因为每个启动的硬件 profile 都需要后端准入。`runtime/hardware/manager/backend_catalog.cpp` 组装 descriptor；`BackendCatalog` 拒绝空类型和重复类型；`Registry` 解析选定 binding，检查设备类型、primitive 与 feed rate，为每个设备构造适配器，并在进程生命周期内持有这些实例。选定的后端不可用时，启动会以明确的配置错误终止。Registry 向感知模块提供只读遥测，只向 Dispatcher 提供受控的可变访问。

必需的 Demo 适配器和模拟器 descriptor 位于 `runtime/hardware/backends/`。可选设备与传输适配器仍位于 `ext/backends/`，通过同一个 catalog 注册。MuJoCo 目前是独立安装的模拟器进程；其 EOS descriptor 在构建时编入运行时。安装模拟器版本会提供进程和配置，不会向运行中的 `eosd` 注入代码。



### 软件包边界

软件包中心当前的 `extension` 类型安装独立进程并生成 profile，不修改运行中的 backend catalog。未来的 `backend` 类型会把代码加载进 `eosd`；在具备版本化后端 ABI、可信发布策略、加载隔离、descriptor 校验和准入测试之前，不开放该类型发布。管理器必须在构造 factory 前验证精确 ABI 和支持的 EOS 契约。未知或不兼容的软件包应拒绝加载；版本切换只能在 EOS 新进程启动后生效。v1 不支持运行时热切换。

这一划分保留稳定的软件包 ID 与 manifest：以后注册表新增 `backend` 类型，无需更改安装或资产校验协议。由于其代码运行在硬件权威进程内，后端软件包仍需要 EOS 专用的激活和审核流程。

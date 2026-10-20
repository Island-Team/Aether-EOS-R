# Hardware runtime

[Runtime overview](README_cn.md) links the integrated runtime module set.

Runtime module at `src/eos/runtime/hardware/` — the sole owner of every south-bound backend instance. It parses a resolved binding, instantiates concrete adapters behind `eos/hardware/backend.h`, admits them at startup, forwards the bridge tick, and aggregates device health. It knows nothing north-bound: no JSON and no contract reads.

## What exists now

**Binding parsing** (`binding.cpp`, toml++). 每个 `[profile.<name>]` 包含 robot metadata 和 `[[device]]`。EOS 自有 primitive catalog 位于 `binding.cpp`；每个 `[[device.primitive]]` 都有精确 version 与 description，reference-aware motion 还声明允许的 `references`，absolute spatial target 声明 `absolute_frame`。Numeric limits 与 reference policy 在 startup admission 后通过 body discovery 发布，并由 Dispatcher 使用。Robot identity 通常来自 profile，device ID 按 binding 顺序确定为 `<class>-<index>`；未知键和不一致声明 fail closed。

**Mode resolution** (`mode.cpp`). Requires exactly one `[[mode.binding]]` from `etc/eos/eos.toml`. Additional robots use separate EOS gRPC endpoints and, when browser access is needed, separate adapter listeners. `[mode.primitives] expose_operator` defaults false.

**Identity and admission** (`identity.cpp`, `admission.cpp`). `Load` 一次性解析并接纳单个 hardware binding。配置的 `robot_id` 跨重启稳定；缺失时生成 process-lifetime UUIDv7。Bridge 分配 `<class>-<index>` device ID，并将 backend type × class 路由到对应 factory。Admission 检查 feed rate 与 primitive policy。`aplay` factory 只验证 ALSA route 与完整 asset byte bound；TTS 是 Dispatcher 中 advanced `speaker.speak` 的 dependency，不是 physical backend dependency。

**Read-only feed seam.** `Device::identity()`, `feeds()`, part metadata, envelope, and health are const. Body perception and gateway can inspect those values but cannot command, clamp, halt, or tick a backend. Mutable backend access is private to bridge Registry and granted to dispatch's Dispatcher for controlled release. This makes bridge the only backend owner without creating a second actuator path.

**Demo-scale typed accessors.** `Base()` / `Arm()` / `Gimbal()` / `Gripper()` / `Speaker()` return const concrete demo views for the first device of that kind and part, or `nullptr`. The cast is gated on backend kind and part kind, so a ROS 2 manipulator never masquerades as the demo arm.

**Tick and health.** `Registry::Tick(dt_s)` 只转发给拥有本地工作的 backend。Demo adapter 不推进 world，而是向拥有独立时钟的 `eos-simulator` 发起有界私有 RPC。模拟器缺失或断开时，Demo health 为 unavailable、命令被拒绝且 camera 不返回 frame，但 EOS 不会终止。Per-device health 是 `Backend::ReadHealth()` 返回的 backend-neutral `alive`/`admitted` 结果；只有显式 `safety.estop` primitive 设置 process-wide estop latch。

**Direct speaker path.** `speaker.cpp` 构造不依赖 model service 的 bridge-owned `aplay` backend。Dispatcher 为 `speaker.play` 解析 asset URI，或为 `speaker.speak` 获取合成结果，校验一个完整 16 kHz mono PCM WAV 后 release atomic `SpeakerCommand`。Backend 再次校验完整 asset，并直接执行 `aplay -q -D DEVICE`；它拥有 cancellation、child reaping 与 playback telemetry。不存在 chunk protocol、独立 playback process 或 ROS 2 speaker path。详见 [Aplay 扬声器后端](../../develop/ext/backends/host_cn.md#aplay-扬声器后端)。

## Deliberately later

Hot-plug (device appear/disconnect handling), the inotify config watch with re-admission, and richer bridge-health/freshness admission in Dispatcher remain future work — see [支撑与侧向模块](../eos-2607_cn.md#支撑与侧向模块). Until then the registry is a startup-time construct: no runtime swapping, no reload.

## Testing

Unit tests with committed and inline bindings are registered as `hardware_tests` (GTest): `cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON && cmake --build build/aether -j && ctest --test-dir build/aether`. They cover strict binding/mode parsing, UUID lifetime, generated per-class IDs, optional data feeds, required health feeds, per-kind defaults and rejections, the committed demo profile and inline bindings, rate admission, typed accessors, tick integration, and explicit estop latching.

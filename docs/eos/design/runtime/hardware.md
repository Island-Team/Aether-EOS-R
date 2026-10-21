# Hardware runtime

[Runtime overview](README.md) describes the integrated dataflow and links the runtime module set.

Runtime module at `src/eos/runtime/hardware/` — the sole owner of every southbound backend instance. It parses a resolved binding, instantiates concrete adapters behind `eos/hardware/backend.h`, admits them at startup, forwards the hardware tick, and aggregates device health. It knows nothing northbound: no JSON and no contract reads.

## What exists now

**Binding parsing** (`binding.cpp`, toml++). Every file is profiled: `[profile.<name>]` contains robot identity/model/vendor/description metadata plus `[[device]]` entries with per-device feeds, `part_id`, description/tags, static per-kind config, and optional staged scenes. Common structure, class geometry, and the EOS-owned primitive catalog stay in `runtime/hardware/`. Each backend descriptor under `ext/backends/` owns its settings parser, supported classes and primitives, cross-device checks, and factory. The caller passes a `BackendCatalog` explicitly to binding parsing and registry admission. Every used `[[device.primitive]]` has a required exact version and device-specific `description`; reference-aware motion also declares allowed `references` and, for absolute spatial targets, `absolute_frame`. An advanced primitive requires an exact `model_id`, while a direct primitive rejects one. Numeric limits, references, and dependency policy are admitted against that catalog, then projected through body discovery and consumed by Dispatcher for release. `robot_id` and `robot_name` normally come from the selected profile; device IDs are deterministic `<class>-<index>` values. Required keys are required, unknown keys are rejected, and every failure names the file and problem.

**Mode resolution** (`mode.cpp`). Requires exactly one `[[mode.binding]]` from `etc/eos/eos.toml`. Additional robots run as separate EOS processes with distinct gRPC endpoints and, when needed, distinct external adapter listeners. `[mode.primitives] expose_operator` defaults false.

**Identity and admission** (`identity.cpp`, `admission.cpp`). `Load` parses and admits the process's single hardware binding in one barrier. A configured `robot_id` is stable across restarts; omission generates a process-lifetime UUIDv7 and logs a warning. `robot_name` is the human route key and defaults to `robot-0`. The registry assigns `<class>-<index>` device IDs in binding order and invokes the selected backend descriptor's factory. The built-in catalog is composed in `ext/backends/builtin.cpp`; an unavailable optional backend is distinguished from an unknown type. Admission checks every declared feed rate against its configured envelope. Runtime composition then resolves each advanced primitive's exact model ID and refuses launch if it is missing, disabled, or has the wrong capability. The `aplay` factory validates its ALSA route and complete-asset byte bound; TTS remains a Dispatcher dependency of the advanced `speaker.speak` primitive rather than a physical-backend dependency.

**Read-only feed seam.** `Device::identity()`, `feeds()`, part metadata, envelope, and health are const. Body perception and the northbound runtime can inspect those values but cannot command, clamp, halt, or tick a backend. Mutable backend access is private to the hardware `Registry` and granted to dispatch's `Dispatcher` for controlled release. This makes the hardware runtime the only backend owner without creating a second actuator path.

**General device access.** Callers use `Registry::Find` and the backend-neutral telemetry methods. The registry exposes no concrete demo adapter types.

**Tick and health.** `Registry::Tick(dt_s)` forwards only to backends that own local work. Demo adapters do not advance a world; they issue bounded private RPCs to the separately clocked `eos-simulator`. A missing or disconnected simulator makes demo health unavailable, commands reject, and camera reads return no frame without terminating EOS. Per-device health is the backend-neutral `alive`/`admitted` result returned by `Backend::ReadHealth()`. Dispatcher owns the process-wide irreversible estop latch and invokes hardware-owned backends only through their controlled `Halt()` seam.

**Direct speaker path.** The selected `demo` or `aplay` descriptor constructs a hardware-owned speaker backend without a model service. Dispatcher resolves `speaker.play` asset URIs or obtains synthesized output for `speaker.speak`, validates one complete 16 kHz mono PCM WAV, and releases an atomic `SpeakerCommand`. The demo backend acknowledges playback through simulator IPC. The `aplay` backend enforces its complete-asset byte bound, validates the WAV again, and spawns `aplay -q -D DEVICE`; it owns cancellation, child reaping, and playback telemetry. Backend details are in [Aplay speaker backend](../../develop/ext/backends/host.md#aplay-speaker-backend).

## Deliberately later

Hot-plug (device appear/disconnect handling), the inotify config watch with re-admission, and richer hardware-health/freshness admission in Dispatcher remain future work — see [Supporting and side modules](../eos-2607.md#supporting-and-side-modules). Until then the registry is a startup-time construct: no runtime swapping, no reload.

## Testing

Generic fixtures with synthetic bindings and fake backends are registered as `hardware_tests` (GTest). Backend parser and behavior cases run in backend-local tests. Run `cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON && cmake --build build/aether -j && ctest --test-dir build/aether`.

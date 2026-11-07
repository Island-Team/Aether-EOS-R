# Hardware runtime

<!-- docs-tree:start -->
**Location:** [Docs](../../../README.md) / [EOS](../../README.md) / [Design](../README.md) / [Runtime](README.md) / **hardware**

<details>
<summary>Docs tree</summary>

- [docs/](../../../README.md)
  - [eos/](../../README.md)
    - [design/](../README.md)
      - [runtime/](README.md)
        - [northbound/](northbound/README.md)
        - [skills/](skills/README.md)
        - [actionlist.md](actionlist.md)
        - [dispatch.md](dispatch.md)
        - **hardware.md**
        - [init.md](init.md)
        - [kernel.md](kernel.md)
        - [models.md](models.md)
        - [northbound.md](northbound.md)
        - [perception.md](perception.md)
        - [skills.md](skills.md)
        - [store.md](store.md)

</details>
<!-- docs-tree:end -->

[Runtime overview](README.md) describes the integrated dataflow and links the runtime module set.

Runtime module at `adapters/eos-cxx/runtime/hardware/` — the sole owner of every southbound backend instance. It parses a resolved binding, instantiates concrete adapters behind `eos/hardware/backend.h`, admits them at startup, forwards the hardware tick, and aggregates device health. It knows nothing northbound: no JSON and no contract reads.

## What exists now

**Binding parsing** (`binding.cpp`, toml++). Every file is profiled: `[profile.<name>]` contains robot identity/model/vendor/description metadata plus `[[device]]` entries with per-device feeds, `part_id`, description/tags, static per-kind config, and optional staged scenes. Common structure, class geometry, and the EOS-owned primitive catalog stay in `runtime/hardware/`. Each backend descriptor under `runtime/hardware/backends/` or `ext/backends/` owns its settings parser, supported classes and primitives, cross-device checks, and factory. The caller passes a `BackendCatalog` explicitly to binding parsing and registry admission. Every used `[[device.primitive]]` has a required exact version and device-specific `description`; reference-aware motion also declares allowed `references` and, for absolute spatial targets, `absolute_frame`. An advanced primitive requires an exact `model_id`, while a direct primitive rejects one. Numeric limits, references, and dependency policy are admitted against that catalog, then projected through body discovery and consumed by Dispatcher for release. `robot_id` and `robot_name` normally come from the selected profile; device IDs are deterministic `<class>-<index>` values. Required keys are required, unknown keys are rejected, and every failure names the file and problem.

**Mode resolution** (`mode.cpp`). Requires exactly one `[[mode.binding]]` from `etc/eos/eos.toml`. Additional robots run as separate EOS processes with distinct gRPC endpoints and, when needed, distinct external adapter listeners. `[mode.primitives] expose_operator` defaults false.

**Identity and admission** (`identity.cpp`, `admission.cpp`). `Load` parses and admits the process's single hardware binding in one barrier. A configured `robot_id` is stable across restarts; omission generates a process-lifetime UUIDv7 and logs a warning. `robot_name` is the human route key and defaults to `robot-0`. The registry assigns `<class>-<index>` device IDs in binding order and invokes the selected backend descriptor's factory. The built-in catalog is composed in `runtime/hardware/manager/backend_catalog.cpp`; an unavailable optional backend is distinguished from an unknown type. Admission checks every declared feed rate against its configured envelope. Runtime composition then resolves each advanced primitive's exact model ID and refuses launch if it is missing, disabled, or has the wrong capability. The `aplay` factory validates its ALSA route and complete-asset byte bound; TTS remains a Dispatcher dependency of the advanced `speaker.speak` primitive rather than a physical-backend dependency.

The demo adapters are required EOS runtime components. `EOS_BACKEND_DEMO=OFF` is rejected at CMake configuration; optional transport backends retain their own switches.

**Read-only feed seam.** `Device::identity()`, `feeds()`, part metadata, envelope, and health are const. Body perception and the northbound runtime can inspect those values but cannot command, clamp, halt, or tick a backend. Mutable backend access is private to the hardware `Registry` and granted to dispatch's `Dispatcher` for controlled release. This makes the hardware runtime the only backend owner without creating a second actuator path.

**General device access.** Callers use `Registry::Find` and the backend-neutral telemetry methods. The registry exposes no concrete demo adapter types.

**Tick and health.** `Registry::Tick(dt_s)` forwards only to backends that own local work. The simulator-backed adapters do not advance a world; they issue bounded private RPCs to the external MuJoCo service. A missing or disconnected simulator makes backend health unavailable, commands reject, and camera reads return no frame without terminating EOS. Per-device health is the backend-neutral `alive`/`admitted` result returned by `Backend::ReadHealth()`. Dispatcher owns the process-wide irreversible estop latch and invokes hardware-owned backends only through their controlled `Halt()` seam.

**Direct speaker path.** The `aplay` descriptor constructs the hardware-owned speaker backend without a model service. Dispatcher resolves `speaker.play` asset URIs or obtains synthesized output for `speaker.speak`, validates one complete 16 kHz mono PCM WAV, and releases an atomic `SpeakerCommand`. The backend enforces its complete-asset byte bound, validates the WAV again, and spawns `aplay -q -D DEVICE`; it owns cancellation, child reaping, and playback telemetry. Backend details are in [Aplay speaker backend](../../develop/ext/backends/host.md#aplay-speaker-backend).

## Deliberately later

Hot-plug (device appear/disconnect handling), the inotify config watch with re-admission, and richer hardware-health/freshness admission in Dispatcher remain future work — see [Supporting and side modules](../eos-2607.md#supporting-and-side-modules). Until then the registry is a startup-time construct: no runtime swapping, no reload.

## Testing

Generic fixtures with synthetic bindings and fake backends are registered as `hardware_tests` (GTest). Backend parser and behavior cases run in backend-local tests. Run `cmake -S . -B build/aether -DISLAND_BUILD_TESTS=ON && cmake --build build/aether -j && ctest --test-dir build/aether`.

## Backend manager and package boundary

The backend manager is part of EOS runtime because every launched hardware profile needs backend admission. `runtime/hardware/manager/backend_catalog.cpp` assembles descriptors; `BackendCatalog` rejects duplicate or empty types; `Registry` parses the selected binding, checks declared classes, primitives and feed rates, constructs one adapter per device, and owns those adapters for the process lifetime. Startup fails with a named configuration error when a selected backend is unavailable. The registry exposes read-only telemetry to perception and controlled mutable access to Dispatcher.

The required demo adapter and the simulator descriptor live in `runtime/hardware/backends/`. Optional device and transport adapters remain in `ext/backends/` and register through the same catalog. MuJoCo is currently a separately installed simulator process; its EOS descriptor is compiled into the runtime. Installing its simulator release supplies the process and config, not new executable code inside `eosd`.

Demo and MuJoCo share the simulator transport implementation but declare their supported capabilities separately. The [MuJoCo design](../mujoco.md) covers the separate process, physics, observations, and benchmark scoring.

## Package boundary

The package center's current `extension` kind installs a separate process and generates a profile. It must not edit the running backend catalog. A future `backend` package kind would install code loaded by `eosd`; keep that kind unpublished until a versioned backend ABI, trusted release policy, loader isolation, descriptor validation, and admission tests exist. The manager must verify an exact ABI and supported EOS contract before factory construction. Unknown or incompatible packages fail closed, and a version switch takes effect only after a new EOS process starts. There is no runtime hot swap in v1.

This split preserves stable package IDs and manifests: the package registry can add `backend` as a new kind later without changing the installation or asset verification protocol. A backend package still needs an EOS specific activation and review path because its code runs within the hardware authority.

# Develop with EOS

<!-- docs-tree:start -->
**Location:** [Docs](../../README.md) / [EOS](../README.md) / [Develop](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../README.md)
  - [eos/](../README.md)
    - [develop/](README.md)
      - [ext/](ext/README.md)
      - [hardware.md](hardware.md)
      - [ros2.md](ros2.md)

</details>
<!-- docs-tree:end -->

[中文](../../../docs_cn/eos/develop/README_cn.md) · [EOS documentation catalog](../README.md)

For the default Rust process set, use the [v2 build and launch guide](build-and-run.md) and [hardware development guide](hardware.md). The remaining C++ module guides describe the unlaunched side chain.
The [v2 ROS 2 integration note](ros2.md) defines the target adapter behavior for a future driver port.

This is the development path after [setting up and operating EOS](../usage/README.md). Read in order: locate the module, check its boundary, inspect its [interface](../interfaces/README.md), then use its design page for behavior and failure cases. The default Rust workspace is under `crates/aether-*`. `adapters/eos-cxx/` contains the opt-in C++ side chain and the active native skill SDK. The published `share/eos/contracts/eos/` and `share/eos/schemas/eos/` resources are also consumed by Rust and EGIA; their identities remain available when an old C++ process is retired.

| Default Rust concern | Crates |
| --- | --- |
| Shared utilities and graph admission | `aether-common`, `aether-node-profile` |
| Typed hardware and MuJoCo transport | `aether-hal`, `aether-mujoco` |
| Command admission and process ownership | `aether-dispatcher`, `aether-runtime` |
| Client transport and services | `aether-control`, `aether-http-bridge`, `aether-config-server`, `aether-tui` |

## Opt-in C++ side chain and historical modules

The module map below documents the retained C++ source and historical designs. It does not describe the current Rust execution path for Workflow, ActionList, or model-weight execution. The old north HTTP server retains its distinct published REST, SSE and WebSocket routes while its opt-in interface remains supported.

## 1. Find the owner

| Concern | Source to inspect first | Design and change guide |
| --- | --- | --- |
| Startup, configuration and composition | `adapters/eos-cxx/runtime/init/`, `adapters/eos-cxx/bin/eos-2607.cpp` | [Initialization](../design/runtime/init.md) |
| Clocks, paths, logging, cancellation and execution | `adapters/eos-cxx/lib/` | [Shared libraries](../design/lib.md) |
| Backend selection, device identity and feed admission | `adapters/eos-cxx/runtime/hardware/`, `adapters/eos-cxx/include/eos/hardware/backend.h` | [Hardware](../design/runtime/hardware-legacy.md) |
| Device adapters | `adapters/eos-cxx/ext/backends/` | [Backend extensions](ext/README.md) |
| Read-only robot and world projections | `adapters/eos-cxx/runtime/perception/`, `adapters/eos-cxx/runtime/store/` | [Perception](../design/runtime/perception.md), [stores](../design/runtime/store.md) |
| External inference and canonical model results | `adapters/eos-cxx/runtime/models/`, `adapters/eos-cxx/ext/models/` | [Models](../design/runtime/models.md) |
| Primitive validation, command admission and actuator release | `adapters/eos-cxx/runtime/dispatch/` | [Dispatch](../design/runtime/dispatch.md) |
| Workflow DAG, facts and authoritative journal | `adapters/eos-cxx/runtime/actionlist/` | [ActionList](../design/runtime/actionlist.md) |
| Skill records, scheduling, built-in and native execution | `adapters/eos-cxx/runtime/skills/` | [Skills](../design/runtime/skills.md) |
| gRPC Gateway, Skill and Workflow services | `adapters/eos-cxx/runtime/northbound/` | [Northbound](../design/runtime/northbound.md) |
| Separate HTTP, simulator and REPL processes | `adapters/eos-cxx/ext/north_http/`, `adapters/eos-cxx/ext/simulator/`, `adapters/eos-cxx/ext/repl/` | [Extensions](ext/README.md) |

Each module's `CMakeLists.txt` and adjacent `tests/` show its build dependencies and verification scope. Start at the owning folder before changing a caller.

## 2. Respect the boundary

[Runtime overview](../design/runtime/README.md) gives the complete data flow. Initialization validates configuration and composes services before opening the listener. Hardware owns admitted devices and observations. Perception reads observations without releasing actions. Models translate configured provider results into canonical data. Dispatch is the sole primitive release gate; ActionList and skills request effects through it. ActionList owns workflow scheduling and facts; SkillManager owns individual skill executions. Northbound maps those operations to gRPC; the separate HTTP adapter consumes that gRPC interface.

Keep validation and state transitions in their owning module. Cross another module only through its declared dependency or contract. The focused [runtime design pages](../design/runtime/README.md) describe ownership, failure behavior, concurrency, shutdown and tests. [Extension guides](ext/README.md) describe separate processes and backend adapters.

## 3. Check the interface

Use the [interface guide](../interfaces/README.md) in this order:

| Module or boundary | Interface to read before editing |
| --- | --- |
| Initialization and northbound | [gRPC access model](../interfaces/grpc.md), [contract source map](../interfaces/contracts.md) |
| Hardware and dispatch | [Southbound HAL](../interfaces/southbound/README.md), [actions](../interfaces/actions.md) |
| Perception and state | [Body](../interfaces/body.md), [world](../interfaces/world.md) |
| Models and external providers | [Model provider contract](../interfaces/external/models.md) |
| ActionList | [Workflow contract](../interfaces/workflow.md) |
| Skills | [SkillService](../interfaces/skills.md), [native skill ABI](../interfaces/skills/native/README.md) |
| HTTP and simulator extensions | [HTTP/SSE](../interfaces/external/http.md), [simulator](../interfaces/external/simulator.md) |

The shared `lib/` and internal store have no independent process API; read their owning [design pages](../design/lib.md) and [store design](../design/runtime/store.md) before changing their in-process callers.

Edit authored contracts under `share/eos/contracts/eos/`, regenerate derived artifacts and run the checks in [contract architecture](../interfaces/contracts.md#updating-a-contract). A source folder is an implementation location; its public behavior is defined by the relevant contract and interface guide.

## 4. Follow behavior into code

For a bug or algorithm change, start with the owning [runtime design page](../design/runtime/README.md), follow its state transitions and failure cases into the corresponding source folder, then inspect that folder's tests. [Algorithm direction](../design/algorithms.md) covers planned edge compute; use module design pages for implemented behavior. For process-wide symptoms, use the [debugging runbook](../debug/README.md) to trace requests, logs and cursors before editing code.

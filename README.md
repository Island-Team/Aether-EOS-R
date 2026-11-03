<div align="center">
  <img src="docs/assets/eos-logo.png" width="180" alt="IslandOS Aether EOS logo">

  # IslandOS Aether · EOS

  **The embodied execution and safety boundary for intelligent robots**

  [![Rust](https://img.shields.io/badge/runtime-Rust-CE412B?style=flat-square&logo=rust)](https://www.rust-lang.org/) [![C++17](https://img.shields.io/badge/hardware-C%2B%2B17-00599C?style=flat-square&logo=cplusplus)](https://isocpp.org/) [![Linux](https://img.shields.io/badge/platform-Linux-FCC624?style=flat-square&logo=linux&logoColor=black)](https://kernel.org/)

  [中文](README_cn.md) · [Documentation](docs/README.md) · [Island Team](https://www.os-island.com)
</div>

**Island Team** develops **IslandOS Aether**, which separates agent decisions from robot execution. **EOS (Embodied OS)** is its Rust robot runtime. Its v2 MVP turns a validated node graph into discoverable hardware nodes, observations, and controlled actions. Every device command passes through one dispatch gate before an adapter can release it to a backend. The old C++ skills and workflow stack remains an unlaunched side chain.

The separate **EGI Agent (EGIA)** has its own core in the shared node graph; its execution is outside the current MVP. Operator clients use the separate v2 HTTP bridge or REPL. The old gRPC and HTTP/SSE interfaces belong to the C++ side chain. The [Island introduction](docs/island/README.md) explains these components, the [public website](https://www.os-island.com), and the local package and launch tools.

## Read this repository in order

1. **Meet Island:** [product and repository map](docs/island/README.md), then the [documentation catalog](docs/README.md).
2. **Use EOS:** [setup, configuration, launch, and stop](docs/eos/usage/README.md). Simulation uses a separately installed MuJoCo package; IsOS manages the deployment and EOS supervises the simulator child.
3. **Develop EOS:** [module ownership and change boundaries](docs/eos/develop/README.md).
4. **Use the APIs:** [interface map](docs/eos/interfaces/README.md), then the interface page for the module you are changing.
5. **Debug an implementation:** [runtime design](docs/eos/design/runtime/README.md), then the detailed module design and [algorithms](docs/eos/design/algorithms.md).

The [documentation home](docs/eos/README.md) keeps this order and links to the complete reference. Follow the use guide first if this is your first run.

## Build EOS

The default `./build.sh` compiles the Rust v2 workspace in release mode. Install Rust, Cargo, CMake 3.22+, and a C++17 compiler for the narrow CXX adapter. `./scripts/test_rust_workspace.sh` runs formatting, lint, and workspace tests; `--bench` adds the fixed dispatcher benchmark. Output is saved under `logs/test/`. Use `bin/eos-config validate --file etc/eos/backends/mock-cxx.toml` to check a graph without opening devices.

For process composition, use `isos init`, `isos serve`, `isos config apply <graph>`, and `isos run`; `isos deployment stop` stops graph children and `isos stop` shuts down the manager too. The [MuJoCo graph template](examples/v2/mujoco-isos.toml) includes simulator, EOS, HTTP bridge, and config-server nodes. See the [v2 build and launch guide](docs/eos/develop/build-and-run.md) for source and installed paths, smoke commands, and physical hardware limits. The legacy C++ build is available only with `./build.sh --cpp-side-chain`.

## Rust workspace map

The virtual Cargo workspace follows the top-level `crates/` layout used by `../Island`. All ten crates are version `0.2.0` and use the `aether-` package prefix:

| Crates | Responsibility |
| --- | --- |
| `aether-common`, `aether-node-profile` | Bounded time and I/O utilities; registered node types and graph validation. |
| `aether-hal`, `aether-mujoco` | Typed camera, base, and manipulator interfaces; isolated CXX mock and MuJoCo adapters. |
| `aether-dispatcher`, `aether-runtime` | One actuator command gate, durable outcomes, adapter ownership, and the control process. |
| `aether-control`, `aether-http-bridge`, `aether-config-server`, `aether-tui` | Bounded client transport, loopback HTTP, read-only graph service, and REPL. |

The Rust executables are `aether-runtime`, `aether-profile`, `aether-http-bridge`, `aether-config-server`, and `aether-tui`. The public `eosd`, `eos-config`, `eos-north-http`, and `eos-cli` commands launch these binaries. IsOS manages the process graph; its validator default is `aether-profile` and its override is `ISOS_AETHER_PROFILE_BIN`. See the [build and launch guide](docs/eos/develop/build-and-run.md) and [node interface](docs/eos/interfaces/hardware.md).

`src/eos/` contains the opt-in C++ hardware and legacy runtime side chain. It is outside the default Rust build and install path. Its older design and deployment guides remain under [EOS documentation](docs/eos/README.md) for callers that still use that side chain.

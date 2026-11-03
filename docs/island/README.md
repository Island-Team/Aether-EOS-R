# Island and Aether

<!-- docs-tree:start -->
**Location:** [Docs](../README.md) / [Island](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../README.md)
  - [island/](README.md)
    - [interfaces/](interfaces/README.md)
    - [contracts_aether.md](contracts_aether.md)
    - [conventions.v1.3.0.md](conventions.v1.3.0.md)
    - [layout.md](layout.md)

</details>
<!-- docs-tree:end -->

[中文](../../docs_cn/island/README_cn.md) · [Documentation catalog](../README.md)

**Island Team** develops **IslandOS Aether**, a platform that connects an intelligent agent to simulated and physical robots. Aether keeps cognition and physical execution in separate processes. The **EGI Agent (EGIA)** interprets operator intent, plans work, and requests typed actions. The **Embodied OS (EOS)** admits robot devices, publishes read-only observations, runs skills, and checks commands before an adapter reaches hardware. Their shared contract defines what crosses that boundary. This checkout contains EOS and the product-level files needed to build it; it does not contain the other services.

## Core platform map

| Project or tool | Role | Where to go |
| --- | --- | --- |
| **IsOS** | Local deployment manager for the applied EOS/EGIA node graph and separately configured model-like services; owns their process lifecycle and the workbench. | [IsOS repository](https://github.com/Island-Team/isos) |
| **Island-web** and [www.os-island.com](https://www.os-island.com) | Public website and browser experience, including the IsOS manager and EGIA operator console. | [Island-web repository](https://github.com/Island-Team/Island-web) |
| **Island-port** | Web and service gateway, including the package catalog API consumed by IsOS. | [Island-port repository](https://github.com/Island-Team/Island-port) |
| **Island-egia** | Agent cognition, planning, memory, and orchestration; communicates with EOS through the shared contract. | [Island-egia repository](https://github.com/Island-Team/Island-egia) |
| **Island-eos / Aether-EOS-R** | This development checkout and its separately maintained public EOS release repository. EOS owns robot execution and the command gate. | [EOS guide](../eos/README.md) · [Public source](https://github.com/Island-Team/Aether-EOS-R) |
| **Island-models** | Separately served speech, vision, language, and other model capabilities used by the Agent or EOS when configured. | [Organization overview](https://github.com/Island-Team) |
| **Island-Base and robot projects** | Device-specific adapters, controllers, and hardware integration that stay outside the portable EOS runtime. | [Organization overview](https://github.com/Island-Team) · [Organization overview](https://github.com/Island-Team) |

This is the core Aether map, not an inventory of every Island Team project. The [organization overview](https://github.com/Island-Team) lists additional research, simulation, and robot repositories.

## Tools in this checkout

The default EOS runtime uses the `eosd`, `eos-north-http`, `eos-config`, and `eos-cli` wrappers around Rust binaries. IsOS applies its v2 graph and owns the outer process lifecycle. The `island-pkg` client and `island-launch` runtime stack belong to the opt-in C++ compatibility path. Start with the [EOS usage guide](../eos/usage/README.md); use the [legacy package guide](../eos/usage/packages.md) and [manifest contract](interfaces/packages.md) when maintaining that side chain.

## Product-wide references

- [Layer connections and data flow](layer-dataflow.md): typed edges, service bridges, safety routes, failure propagation and stop order.
- [Detailed 0.x implementation TODO](implementation-todo.md): owned tasks, dependencies and acceptance tests.
- [Q01 selected Restart replay](q01-restart.md): bounded model fault and explicit new-run recovery evidence.
- [Q01 integrated first-use design](q01-first-use-design.md): owner flow, frozen fixtures and complete qualification evidence plan.
- [0.x architecture and first-use path](architecture.md): ownership, release targets, lifecycle, compatibility and implementation order.
- [Repository layout](layout.md): what this checkout contains and where source, contracts, build output, and local state belong.
- [Coding and documentation conventions](conventions.v1.3.0.md): rules shared across Island repositories.
- [Aether–EOS contract ownership](contracts_aether.md): the Agent–EOS boundary and its authored contract source.

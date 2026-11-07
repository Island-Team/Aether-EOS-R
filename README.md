# IsOS (Island OS)

[中文](README_cn.md) · [Documentation](docs/README.md)

IsOS is the Island OS launcher and manager: it owns deployment setup, validation, and the applied graph revision before supervising other processes. **EI (Embodied Intelligence)** supplies cognition and its safety gate. **EOS (Embodied OS)** supplies embodied execution, hardware adapters, and the Dispatcher. **IsosUI** is the browser workbench. These domains share one `isos` graph core while EI and EOS retain separate processes and explicit transports. MuJoCo stays outside this repository as the verified `island/mujoco-sim@0.1.1` package.

The unified source version is **0.2.1**. New graphs use format `0.2.0` and `isos.<layer>.<name>@v0.2` type IDs. The revision-checked migration and Apply path is enabled; its final public qualification remains in progress. **No v0.2.1 product tag or release has been published.** See the [unification design](docs/island/unification-v0.2.1.md) and [release draft](docs/island/release-v0.2.1-draft.md).

## Repository map

| Path | Owner |
| --- | --- |
| `crates/isos/` | Manager, graph migration, packages, process supervision, and CLI. |
| `crates/aether-*/`, `adapters/eos-cxx/`, `share/eos/` | EOS Rust runtime, optional C++ sidechain, and contracts. |
| `ei/` | EI Python project and compatibility commands/imports for EGIA. |
| `ui/` | Browser workbench and its own fixture matrix. |
| `docs/{isos,ei,eos,isosui}/`, `docs_cn/{isos,ei,eos,isosui}/` | Owner documentation in English and Chinese. |

The root Cargo workspace builds the manager and EOS crates. Root CMake composes the Rust binaries, EI distribution, optional UI build, and opt-in native EOS side chain. Follow the [IsOS usage guide](docs/isos/usage/README.md), [EI guide](docs/ei/README.md), [EOS guide](docs/eos/usage/README.md), and [UI guide](docs/isosui/README.md). The [source provenance record](docs/island/source-provenance-v0.2.1.md) maps imported histories and fixture ownership.

## Development

Follow the [repository conventions](docs/island/conventions.v1.3.1.md). Use focused `refactor(v2)/<topic>` branches and save complete checks under ignored `logs/test/<run-id>/`. Before an integration PR, run the relevant Rust formatting, Clippy and tests; EI Ruff, Pyright, import rules and tests; UI typecheck, tests and both builds; CMake/CTest; fixture checks; and the bilingual link audit. A clean-home MuJoCo and Stop replay is required before publication. The historical public EOS 0.2.0 release remains documented under [EOS release evidence](docs/eos/maintenance/release.md).

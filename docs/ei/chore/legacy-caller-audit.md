# Legacy caller audit (2026-11-04)

[中文](../../../docs_cn/ei/chore/legacy-caller-audit.md)

This audit separates unused runtime implementation from compatibility code that still has a caller. It does not change the EOS, IsOS, or MuJoCo wire contracts.

| Candidate | Caller evidence | Decision |
|---|---|---|
| `src/egia/contracts/perception/body/vision.py` | Re-exports `VisionObjectDetection` and `VisionSnapshot` from the current world-perception contract. Python import compatibility is its purpose; the file is part of the packaged `src/egia` tree. | Retain until a separately versioned API retirement. |
| `ISLAND_BUILD_ECOS` CMake option | `tests/system/test_install_layout.py` configures it explicitly; CMake still accepts it as a deprecated build option. | Retain with the test. |
| `src/island/boot.py` | `islandctl` validation and `tests/system/test_product_launcher.py` use its composition and path checks. The public `bin/island` launcher now forwards to `isos start`. | Retain the validation implementation; document the launcher distinction. |
| `island.eos.v1` generated protobuf bindings | EGIA still uses Gateway discovery/resources and SkillService. Primitive execution uses `island.eos.v2.CapabilityService`. | Retain published wire identities. |

`bin/island --config` is rejected by the current launcher. Standalone EGIA uses `bin/egia --island-config <path>`; IsOS deployments use an applied graph. The English and Chinese layout guides now say so.

## Current baseline

- Commands: `bin/island` forwards to `isos start`; `bin/egia` starts the standalone runtime; `bin/islandctl validate` checks the historical product layout. Sources are the wrappers and `src/island/ctl.py`.
- Installation: `CMakeLists.txt` installs `egia`, `egia-config`, available compatibility wrappers, configuration TOML, shared resources, factory defaults, `island_admin`, and Markdown docs; `egia_ext` is opt-in. The EGIA Python runtime is built as a wheel by `egia-dist`.
- Routes: `src/egia/io/northbound.py` defines `/api/aether/egia/health`, backend, chat, missions, review, context, model I/O, primitives, skills, perception and prompt routes. The contract is `docs/ei/northbound.md`.
- Graph versions: EGIA's reader feature fixture advertises `0.1.0` and `1.0.0`. IsOS owns graph persistence and migration; the EGIA reader must not rewrite a saved graph.

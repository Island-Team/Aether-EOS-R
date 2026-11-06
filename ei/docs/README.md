# EI documentation in the unified IsOS repository

[中文](../docs_cn/README.md) · [IsOS documentation](../../docs/README.md)

EI (Embodied Intelligence) source lives under `ei/`. IsOS owns configuration, graph revision, package admission, and launch; EI owns its intelligence runtime and local safety checks. EOS owns actuator admission and hardware execution. The current EI code still contains some EGIA-named compatibility paths; the target removes them in a coordinated contract migration. The [unification plan](../../ARCHITECTURE-QUESTIONS.md) records that boundary.

Start with the current [EI overview](../../docs/ei/README.md), [build and run guide](../../docs/ei/develop/build-and-run.md), [core modules](../../docs/ei/core/modules.md), and [IsOS configuration interface](../../docs/ei/interfaces/isos-config.md). Historical EGIA layout and qualification documents remain in this subtree as dated evidence; use the root `docs/ei/` tree for current owner documentation.

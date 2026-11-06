# IsOS 0.2.1 release draft

**Status: preparation only, 2026-11-06. Do not tag or publish this release.**

IsOS (Island OS) brings its manager, EI (Embodied Intelligence), EOS (Embodied OS), and the IsosUI workbench into one source tree and one `isos` graph core. EI is the new product name for EGIA. EOS remains the embodied execution process with its own Dispatcher and hardware boundary; EI retains its own process and safety gate. MuJoCo is installed separately as the verified `island/mujoco-sim@0.1.1` package.

## Operator changes

- New graphs use format `0.2.0` with a single `isos` core. Manager nodes retain their layers; EOS nodes use `eos_*` layers, EI nodes use `ei_*` layers, and browser nodes use `ui`.
- New type IDs use `isos.<layer>.<name>@v0.2`. Graph format, HTTP, socket, package, protobuf, and simulator versions remain independently versioned.
- EI is the displayed and documented name. Previously published `egia` commands and Python imports remain compatibility aliases through 0.2.1.
- Saved `0.1.0` graphs remain available for read-only inspection. Migration must preview a complete candidate, reject unknown or ambiguous mappings with a precise path, validate both owner projections, and compare the saved revision before an atomic Apply. Until those gates are implemented and verified, migration Apply must fail closed.

## Carried-forward EOS functionality

The previously published EOS 0.2.0 work includes the Rust runtime and five launcher binaries, registered device discovery, Dispatcher admission and journaled outcomes, health and capability services, direct MuJoCo child supervision and owner-only socket adapters, and browser-facing results. The [EOS 0.2.0 release evidence](../eos/maintenance/release.md) is historical qualification for that separate release; it does not qualify the unified 0.2.1 tree.

## Installation and limits

Build the unified repository and verify the external MuJoCo package receipt before selecting a scene. Use the IsOS manager to Validate and Apply a supported graph, then inspect EI and EOS readiness. Issue control only through the existing EI safety and EOS Dispatcher route. Safe Stop must finish admitted actuators before the simulator child is reaped. Exact commands and supported platforms will be filled from the final clean-home replay receipt.

## Release gate (pending)

Record the reviewed private integration commit, public source commit and asset digest, migration and rejection tests, Rust/CMake/EI/UI checks, bilingual links, and the clean-home camera/control/failure/restart/Stop replay under `logs/test/<run-id>/`. Review the public source disclosure, preserve the historical EOS 0.2.0 release and legacy IsOS/IsosUI assets, and verify Island-web's reviewed source pin. Only after those checks may a separate release decision create a `v0.2.1` tag or product release.

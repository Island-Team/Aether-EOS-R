# Shared skill authoring

This tree holds product skills outside the EOS runtime implementation:

- `external/` is the fail-closed placeholder for future app-like workers.
- `native/` is a local, Git-ignored directory for trusted deployment skills that use EOS core interfaces directly.

Native product skills are linked explicitly by the product composition root and may use private EOS interfaces. Generic supervision and provider hosting remain in `adapters/eos-cxx/runtime/skills/`. The external profile remains unimplemented.

When local native sources are present, `native/skills.cpp` is their registration root. A clean checkout has no product skills; the generic skill runtime remains available.

Copy the tracked [`examples/skills/local-native/`](../../examples/skills/local-native/) scaffold into `native/` on a clean checkout, then edit its `CMakeLists.txt` and `RegisterSkills` function as you add skills. If `native/` already contains local skills, merge the scaffold manually so those files are not overwritten. See the [local deployment guide](../../docs/eos/usage/configure.md).

Built artifacts belong in the build or installation tree, not under `share/`.

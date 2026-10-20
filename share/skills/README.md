# Shared skill authoring

This tree owns reviewed product skills outside the EOS runtime implementation:

- `external/` is the fail-closed placeholder for future app-like workers.
- `native/` contains trusted product skills that use EOS core interfaces directly.

Native product skills are linked explicitly by the product composition root and may use private EOS interfaces. Generic supervision and provider hosting remain in `src/eos/runtime/skills/`. The external profile remains unimplemented.

`native/skills.cpp` is the authoritative registration root. The current product composition registers `observe_human`, `interact_waving_owner`, `interact_fallen_owner`, `interact_furniture_owner`, `head_find_trash`, `navigate_waypoint`, `pick_trash`, and `register_host`. A directory or README alone does not make a skill executable: `native/place_trash/` intentionally documents an unimplemented placeholder and is not registered or compiled.

Built artifacts belong in the build or installation tree, not under `share/`.

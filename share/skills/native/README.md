# Native shared skills

Native shared skills are trusted product implementations that use EOS core interfaces directly. They are compiled into an explicitly registered product provider rather than loaded through the public SDK module boundary.

Generic provider, scheduler, and execution infrastructure remains under `src/eos/runtime/skills/`; only product-specific implementations belong here. Maintained product definitions declare exclusive whole-execution claims for their explicit actuator device arguments; primitive-level leases are an additional safety boundary, not the only serialization mechanism.

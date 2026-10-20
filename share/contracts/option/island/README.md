# Optional Island contract boundary

No executable, workflow definition, registry state, or skill schema belongs in this optional contract directory. Island-maintained shared skill source lives under `share/skills/native/`; `share/skills/external/` remains a fail-closed placeholder. EOS exposes the native author ABI in `eos/skill/abi.h`, loads explicit `*.skill.so` artifacts at startup, and validates every primitive call against the canonical schemas outside this optional tree.

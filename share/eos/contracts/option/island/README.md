# Optional Island contract boundary

No executable, workflow definition, registry state, or skill schema belongs in this optional contract directory. Deployment-owned skill source may live in the ignored `share/eos/skills/native/` directory; `share/eos/skills/external/` remains a fail-closed placeholder. EOS exposes the native author ABI in `eos/skill/abi.h`, loads explicit `*.skill.so` artifacts at startup, and validates every primitive call against the canonical schemas outside this optional tree.

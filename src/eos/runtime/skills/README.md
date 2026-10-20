# Skill runtime layout

This directory contains EOS-owned multi-runtime supervision and provider hosting code, not authored skill packages.

```text
skills/
├── core/       provider-neutral metadata, Provider, Catalog, and shared model
├── manager/    SkillManager, runtime registry, scheduler, records, events, logging, and tests
├── builtin/    explicitly registered coroutine skills and capability wrappers
├── native/     trusted in-process C ABI v2 loader and executor
├── managed/    excluded placeholder for compiled state machines
└── external/   fail-closed placeholder for supervised workers
```

`SkillManager` supervises one or more logical `SkillRuntime` registrations. Every exact `{name, version}` has one owning runtime, duplicate identities reject startup, and provider-context claims are scoped by the owning runtime ID. Device and resource claims remain global across runtimes.

The public native authoring ABI lives in `src/eos/include/eos/skill/abi.h` and is installed as `<eos/skill/abi.h>`. Each authored native skill lives in its own deployment-owned directory and builds as a separate `MODULE` target linked to `eos_skill_api`, not runtime internals.

Built-in and native providers are executable. The managed folder is not added by the skills build, and external contributes only a non-executable interface target; neither can admit or launch packages. See [`docs/eos/runtime/skills.md`](../../../../docs/eos/design/runtime/skills.md) and its focused module pages for the ownership and behavior contracts.

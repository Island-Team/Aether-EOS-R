# Build, install, and test a native skill

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Interfaces](../../README.md) / [Skills](../README.md) / [Native](README.md) / **build install test**

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [interfaces/](../../README.md)
      - [skills/](../README.md)
        - [native/](README.md)
          - [abi.md](abi.md)
          - **build-install-test.md**
          - [builtin.md](builtin.md)
          - [capabilities.md](capabilities.md)
          - [community.md](community.md)
          - [example.md](example.md)
          - [package-abi.md](package-abi.md)

</details>
<!-- docs-tree:end -->

[简体中文](../../../../../docs_cn/eos/interfaces/skills/native/build-install-test_cn.md)

[Authoring overview](README.md) · [Complete example](example.md) · [ABI contract](abi.md)

## Standalone CMake project

Install EOS, provide nlohmann JSON 3.11, and consume its CMake package:

```cmake
cmake_minimum_required(VERSION 3.22)
project(example_skill LANGUAGES CXX)

find_package(EOSSkill CONFIG REQUIRED)

eos_add_skill(
  TARGET example_skill
  OUTPUT_NAME example
  SOURCES example.cpp
)
```

`EOS::SkillCpp` supplies C++17 `<eos/skill/sdk.hpp>` and nlohmann JSON; `EOS::SkillABI` supplies only `<eos/skill/abi.h>`. `eos_add_skill` links `EOS::SkillCpp`, creates a hidden-visibility `MODULE` named `example.skill.so`, exports only `eos_skill_library_query_v2`, writes build artifacts to `build/skills`, and installs under `lib/island/skills`. Use `NO_INSTALL` or `INSTALL_DESTINATION` when packaging requires it.

For a native tool that checks ABI compatibility before loading a package, link the optional `EOS::SkillSDK` shared target and include `<eos/skill/support.h>`. Its header check is intentionally shallow; only the EOS loader can admit a skill.

Inside EOS, the same imported-style targets and helper are available. A raw-C target may link `EOS::SkillABI`, but must reproduce the module suffix, visibility, and single-symbol export policy itself.

Verify the artifact before deployment:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/island
cmake --build build
nm -D --defined-only build/skills/example.skill.so
```

The final command must show only `eos_skill_library_query_v2`.

## Install and load

In the opt-in C++ side chain, an enabled package module selects a verified release skill directory from `isos.lock`, and `eos-skill-worker` loads it; source checkouts can use `--skills-dir` or `EOS_SKILLS`. That C++ loader discovers `*.skill.so` recursively. **R04 target Rust path:** IsOS selects exact P01-verified library files through `settings.skill_libraries` and a manifest-listed `isos/skill-selection.json`; the default Rust runtime neither accepts `--skills-dir`/`EOS_SKILLS` as authority nor scans a directory. Invalid ABI, descriptor, schema, declaration, duplicate identity, or unavailable declared callback owner rejects the entire catalog. Restart EOS after changing the selection; hot reload is unavailable. Package installation alone does not enable a skill.

Test handlers against a fake host for success, semantic rejection, malformed host responses, cancellation, missing optional observability callbacks, and exactly-once completion. Then load the module through the native runtime and verify descriptor identity, capability enforcement, result validation, and the query symbol. Exercise logs/checkpoints without making their acceptance a condition of the skill result.

Current admission bounds include 256 skills per library and 64 primitives, resources, SIGNALs, claims, and sources per descriptor. Individual schemas, arguments, and results are limited to 1 MiB. Prefer substantially smaller contracts.

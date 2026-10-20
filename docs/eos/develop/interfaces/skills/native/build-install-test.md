# Build, install, and test a native skill

[简体中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/build-install-test_cn.md)

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

Inside EOS, the same imported-style targets and helper are available. A raw-C target may link `EOS::SkillABI`, but must reproduce the module suffix, visibility, and single-symbol export policy itself.

Verify the artifact before deployment:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/opt/island
cmake --build build
nm -D --defined-only build/skills/example.skill.so
```

The final command must show only `eos_skill_library_query_v2`.

## Install and load

EOS chooses the native skill directory from `--skills-dir`, then `EOS_SKILLS`; without either, the catalog is empty unless the repository wrapper supplies its build or installed directory. The loader scans only top-level regular, non-symlink `*.skill.so` files in lexical order. Any invalid ABI, descriptor, schema, declaration, duplicate identity, or unavailable primitive rejects the complete configured load. There is no hot reload; restart EOS after install.

Test handlers against a fake host for success, semantic rejection, malformed host responses, cancellation, missing optional observability callbacks, and exactly-once completion. Then load the module through the native runtime and verify descriptor identity, capability enforcement, result validation, and the query symbol. Exercise logs/checkpoints without making their acceptance a condition of the skill result.

Current admission bounds include 256 skills per library and 64 primitives, resources, SIGNALs, claims, and sources per descriptor. Individual schemas, arguments, and results are limited to 1 MiB. Prefer substantially smaller contracts.

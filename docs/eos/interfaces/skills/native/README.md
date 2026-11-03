# Write a native EOS skill

<!-- docs-tree:start -->
**Location:** [Docs](../../../../README.md) / [EOS](../../../README.md) / [Interfaces](../../README.md) / [Skills](../README.md) / [Native](README.md)

<details>
<summary>Docs tree</summary>

- [docs/](../../../../README.md)
  - [eos/](../../../README.md)
    - [interfaces/](../../README.md)
      - [skills/](../README.md)
        - [native/](README.md)
          - [abi.md](abi.md)
          - [build-install-test.md](build-install-test.md)
          - [builtin.md](builtin.md)
          - [capabilities.md](capabilities.md)
          - [community.md](community.md)
          - [example.md](example.md)
          - [package-abi.md](package-abi.md)

</details>
<!-- docs-tree:end -->

[中文](../../../../../docs_cn/eos/interfaces/skills/native/README_cn.md) · [SkillService](../../skills.md) · [Skill runtime](../../../design/runtime/skills.md)

**Start with [who owns the ABI and the 0.x target boundary](host-abi.md).** EOS exposes the host capability ABI; packages export a descriptor and query symbol. The current implementation lives in the opt-in C++ side chain. The default Rust EOS runtime does not yet admit native skill packages. The target Rust supervisor will use one isolated worker per execution and brokered capabilities; skill code never receives direct hardware HAL access.

The installed `EOS::SkillABI` and `EOS::SkillCpp` targets are the authoring interfaces. `EOS::SkillSDK` is the optional shared `libeos_skill_sdk.so.2` companion: `<eos/skill/support.h>` reports the supported ABI and checks a library's fixed header/table layout. It is not a worker RPC client. Python external skills are not admitted today; their planned pybind11 bridge belongs inside a separate supervised Python worker and must call only the brokered execution API. See the [worker architecture](../../../design/runtime/skills/external.md) for the admission and sandbox gates.

Read in first-use order:

1. [Host ABI and package boundary](host-abi.md): ownership, admission, invocation, failure, and stop.
2. [Package authoring](package-abi.md) and [example](example.md): build a reviewed `.skill.so` with `<eos/skill/sdk.hpp>` or `<eos/skill/abi.h>`.
3. [Build, install, and test](build-install-test.md): compile and select an exact package release.
4. [Capability operations](capabilities.md) and [skill community](community.md): invocation-scoped requests and cooperation.
5. [C layout reference](abi.md): raw ABI fields, lifetimes, and optional-tail checks.

The [built-in guide](builtin.md) is for C++ library development and tests, not the production package path.

Skills read declared EOS resources and invoke declared primitives through the skill host. EOS validates declarations, arguments, claims, and execution requests before granting capabilities. Packaged ABI v2 skills use the [worker execution path](../../skills.md#external-skill-execution). Primitive, resource, inference, source, SIGNAL, checkpoint, and optional ABI v2.1 community callbacks use the worker broker. Child-process callbacks remain unavailable to packaged workers; see the worker architecture.

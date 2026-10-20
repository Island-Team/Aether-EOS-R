# Write a native EOS skill

[中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/README_cn.md) · [SkillService](../../skills.md) · [Skill runtime](../../../../design/runtime/skills.md)

EOS supports two in-process C++ authoring paths. **Built-in C++ is preferred** for first-party skills. The C/C++ shared-library ABI v2 remains supported for deployment-owned modules during migration. Neither path creates an external skill process or gives skill code direct access to the hardware HAL.

1. [Built-in skill guide](builtin.md): define and explicitly register a coroutine handler, its schemas, and its declared capabilities.
2. [Legacy ABI v2 guide](legacy-abi-v2.md): build a reviewed `.skill.so` with `<eos/skill/sdk.hpp>` or `<eos/skill/abi.h>`.
3. [Capability use](capabilities.md), [local mailbox IPC](ipc.md), and [examples](example.md): invocation-scoped tools and cooperation.
4. [ABI reference](abi.md) and [build, install, test](build-install-test.md): details for the supported shared-library path.

Skills read declared EOS resources and invoke declared primitives through the skill host. EOS validates declarations, arguments, claims, and execution requests before granting capabilities. External skill execution is [not supported](../../skills.md#external-skill-execution).

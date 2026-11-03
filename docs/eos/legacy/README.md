# Historical EOS documentation

[中文](../../../docs_cn/eos/legacy/README_cn.md) · [EOS catalog](../README.md) · [0.x node catalog](../layers/README.md)

These pages describe the opt-in C++ side chain or published compatibility identities. They remain reachable for installed callers and migration work; they do not define the 0.x target architecture. Preserve published protobuf packages, field numbers, ABI symbols, and socket identities even when an implementation is replaced.

| Historical area | Pages | 0.x disposition |
| --- | --- | --- |
| C++ runtime composition | [runtime overview](../design/runtime/README.md), [initialization](../design/runtime/init.md), [hardware HAL](../design/runtime/hardware-legacy.md), [kernel](../design/runtime/kernel.md), [dispatch](../design/runtime/dispatch.md), [state store](../design/runtime/store.md) | Reference until last-caller and parity checks permit removal. |
| Workflow and ActionList | [ActionList design](../design/runtime/actionlist.md), [WorkflowService contract](../interfaces/workflow.md), [northbound adapter](../design/runtime/northbound/workflow.md), [workflow template](../templates/workflow.md) | Historical contract only. The 0.x target has no workflow engine or graph-authored skill sequence. |
| C++ deployment | [local TOML configuration](../usage/configure.md), [configuration reference](../usage/configuration-reference.md), [backend drivers](../usage/backends.md), [ARM64 guide](../usage/arm64.md), [C++ development](../develop/ext/README.md) | Keep clearly labeled as opt-in C++ guidance. First-time 0.x users start at [Run EOS](../usage/README.md). |
| Published native skill contracts | [SkillService](../interfaces/skills.md), [native ABI](../interfaces/skills/native/abi.md), [authoring](../interfaces/skills/native/README.md) | Compatibility identity; see the active skill boundary design before changing ownership. |

The historical page paths remain in place to protect links. This catalog is the single active index for them; the core, layer, group and node catalog contains only current or target architecture.

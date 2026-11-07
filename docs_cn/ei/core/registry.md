# `egia.core.registry`

[English](../../../docs/ei/core/registry.md) · [核心概览](modules.md)

Registry 是 EOS 可执行 primitive binding 的唯一内存 catalog。Model selection 不属于执行发现，native skill 仍是独立的 metadata catalog。

## 构造与替换

运行时构造将 canonical action schema 与 `Discover` 内嵌的 robot-scoped 和 device-scoped binding 结合。Portable schema 仍对 arguments 具有权威性；EOS 提供该部署的精确 version、target、health、reference、frame、dependency、accepted media type、description 和 numeric limit。

Link 层为诊断保留有效的 degraded 与 unavailable binding，但 registry 只接纳 `health == "ready"`。未知 primitive name 或 version、缺失必需 limit、target 格式错误及 compound identity 重复都会 fail closed。按 backend 重新发现时会原子替换该别名，同时保留其他 robot alias；陈旧代次不会与当前代次合并。

`PrimitiveRegistry.from_contracts` 只是确定性的无硬件测试 fixture helper。运行时 composition 使用已发现 binding，绝不会仅凭 canonical schema 虚构 device 或 primitive。

## Primitive identity 与可见性

可执行 identity 是 `{name, exact SemVer, robot alias, scope, device target}`。Device-scoped 调用要求显式 device ID。Robot-scoped `robot.stop` 与 `safety.estop` 没有 device target。当存在多个当前候选时，resolution 不会推断 target。

Descriptor 还记录 management `binding_kind`、health、支持的 `references`、`absolute_frame`、dependency health、`accepted_media_types`、部署描述、canonical argument schema 和强制 numeric `limits`。Northbound catalog 会原样投影这些字段。

Planner rendering 生成紧凑且带 target 的 signature，例如：

```text
gripper.motion@robot-a/gripper-0 action=<close|open> [force_n=<number[0..10]>]
```

只有 `[primitives].expose_operator = true` 时才包含 operator-tier arm primitive；它们始终不会对 planner 可见。Privileged stop path 仍由 monitor/operator 控制，且 catalog 只接纳 canonical v3 name。

## Native-skill catalog

`SkillService.ListSkills` 提供已排序的 descriptor，包含精确 identity、description、schema、精确 primitive reference 和 resource template。临时不可用会用 degraded snapshot 替换陈旧 claim；格式错误的 metadata 属于不兼容契约。由 planner-hidden primitive 组成的 skill 不会进入 planner guidance。

Skill 描述可能的组合方式，但绝不会进入可执行 primitive registry。EGIA 在 `/skills` 暴露其 metadata，不提供 skill execution route，也不调用 EOS skill lifecycle RPC。

## 消费者

- decision 渲染 planner-visible signature 与精确 target；
- safety 在受治理出口前解析 binding；
- coordination 只为分配给当前 robot/device 的 ready primitive 竞价；
- northbound 发布 `/primitives` 与 metadata-only `/skills` snapshot；
- context 渲染紧凑的 primitive 与 skill guidance。

## 解析条件

精确的当前 target 只解析一次。未知、不可用、重复、含糊或 version 不匹配的 binding 都会 fail closed。调用方提供的 `arguments.command_id` 不是 argument，会在 EOS 出口前被拒绝。Refresh 不保留任何陈旧可调用 entry。

**相关：** [link](link.md)、[decision](decision.md)、[safety](safety.md) 和 [northbound](../northbound.md)。

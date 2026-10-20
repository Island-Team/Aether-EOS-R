# Managed skill 运行时

[Skill runtime 总览](../skills_cn.md) · [SkillManager supervisor](manager_cn.md) · [Skill 编写](../../../../../docs/eos/develop/interfaces/skills/native/README.md) · [English](../../../../../docs/eos/design/runtime/skills/managed.md)

managed runtime 文件夹是 obsolete fail-closed placeholder，不向 SkillManager 提供 `SkillRuntime`。Built-in C++ 已取代它成为 supported in-process authoring direction；本次 redesign 不计划 managed language 或 package format。

## 所有权边界

`src/eos/runtime/skills/managed/` 没有 executable implementation，仅在清理旧 build reference 期间保留。不要在此添加 parser、compiler、interpreter、package format 或 executor。新的 in-process skill 属于 built-in registry；isolated language runtime 属于未来 external boundary。

## 当前行为

该文件夹只提供空 CMake interface target。`eosd` 不链接 managed parser、Python interpreter、compiler、executor、catalog 或 fallback path。任何 `.eos.py` 或看似 managed 的 package 都不能被 discover、admit、register 或 launch。

```mermaid
flowchart LR
  package["managed package"] -->|"not admitted"| placeholder["managed placeholder"]
  placeholder -.->|"no SkillRuntime"| manager["SkillManager"]
```

## 准入与 execution

不存在 managed admission 或 execution API。未出现在 registered runtime 中的 skill identity 会 exact lookup 失败；SkillManager 绝不会根据 filename、extension、language 或 package metadata 猜测 provider。native execution 不是 managed input 的 fallback。

## 日志与调试

placeholder 不启动 worker，也不发出 managed lifecycle record。尝试启动未注册的 managed identity 时，会通过普通 `skills`/northbound unknown-skill path 报告，且不记录 package content。startup composition 中不存在 managed `SkillRuntime`，是权威 debug fact。

## 测试

Build 与 composition test 验证 interface target 不链接 executor，且看似 managed 的 input 不会出现在 aggregate catalog。SkillManager unknown-identity test 验证 fail-closed routing。在实现存在前，executable managed-runtime test 不属于当前范围。

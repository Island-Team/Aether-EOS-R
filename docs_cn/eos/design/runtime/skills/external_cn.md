# External skill 运行时

[Skill runtime 总览](../skills_cn.md) · [SkillManager supervisor](manager_cn.md) · [Skill 编写](../../../../../docs/eos/develop/interfaces/skills/native/README.md) · [English](../../../../../docs/eos/design/runtime/skills/external.md)

external runtime 文件夹为 supervised out-of-process skill 保留 ownership。它是**未来** isolation boundary，不是 built-in logical process 之间使用的 mailbox IPC。当前 fail closed，不向 SkillManager 提供 `SkillRuntime`。

## 所有权边界

`src/eos/runtime/skills/external/` 是未来 process supervisor、sandbox policy、capability-scoped IPC protocol、termination control 与 external SDK bridge 的唯一位置。process creation、IPC framing、capability enforcement 与 lifecycle supervision 必须位于 profile-neutral SkillManager 与 native loading code 之外。

## 当前行为

该文件夹只提供空 CMake interface target。`eosd` 不启动 skill subprocess，不打开 external-skill IPC endpoint，不加载 Python runtime，也不 discover external package。任何 external package 都不能被 admit、register 或 execute。

```mermaid
flowchart LR
  package["external package"] -->|"not admitted"| placeholder["external placeholder"]
  placeholder -.->|"no SkillRuntime"| manager["SkillManager"]
```

## 准入与 execution

不存在 external admission 或 execution API。未出现在 registered runtime 中的 skill identity 会 exact lookup 失败；SkillManager 绝不会 spawn process，也不会从另一个 runtime fallback。看似 external 的 file、inherited descriptor、socket 与 environment variable 均被该 placeholder 忽略。

## 日志与调试

placeholder 不启动 supervisor，也不发出 external lifecycle record。尝试启动未注册的 external identity 时，会通过普通 `skills`/northbound unknown-skill path 报告，且不记录 package 或 environment content。startup composition 中不存在 external `SkillRuntime`，是权威 debug fact。

## 测试

Build 与 composition test 验证 interface target 不链接 launcher，且看似 external 的 input 不会出现在 aggregate catalog。SkillManager unknown-identity test 验证 fail-closed routing。在实现存在前，process、IPC、sandbox 与 forced-termination test 不属于当前范围。

# 编写 built-in skill

[总览](../skills_cn.md) · [Process model](process-model_cn.md) · [IPC](../../../develop/interfaces/skills/native/ipc_cn.md) · [Native legacy guide](../../../develop/interfaces/skills/native/README_cn.md) · [English](../../../../../docs/eos/design/runtime/skills/authoring.md)

Built-in C++ 是首选 authoring model。Native ABI v2 作为 deployment-owned library 的 legacy migration path 暂时保留。

定义 `builtin::Definition`：exact identity、parameter/result JSON Schema、declared primitive/resource/source/IPC channel/claim、reentrancy 和 `Task<Outcome>` handler。definition 在 island registry 中显式注册；不要使用 static initialization。

```cpp
builtin::Definition definition{
    .metadata = {.name = "example", .version = "1.0.0",
                 .parameters_schema = {{"type", "object"}},
                 .returns_schema = {{"type", "object"}}},
    .handler = [](builtin::Execution& execution) -> builtin::Task<builtin::Outcome> {
      if (execution.cancelled()) co_return builtin::Outcome::Cancelled();
      co_return builtin::Outcome::Completed({{"ok", true}});
    }};
registry.Register(std::move(definition));
```

只使用 `Execution` wrapper：primitive start/wait/cancel、resource read、timestamped source、process IPC、checkpoint 与 diagnostic。skill 不得保留 invocation reference，也不得直接访问 Dispatcher、hardware、model internal、filesystem、network 或 workflow state。返回 stable machine-readable reason code；message 是 bounded operator text，不能被 parse。

只声明最小 capability set。使用 fake hook 验证 schema admission、success/rejection/failure、cancellation/timeout、concurrent operation、same-device ordering、mailbox backpressure/expiry、child cleanup 与 log redaction。通过 execution snapshot、journal checkpoint、correlation ID、`journalctl` 和 `eos-skill-journal` debug；不要为了诊断添加 payload logging。

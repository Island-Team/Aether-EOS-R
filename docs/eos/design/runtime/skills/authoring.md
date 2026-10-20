# Authoring built-in skills

[Overview](../skills.md) · [Process model](process-model.md) · [IPC](../../../develop/interfaces/skills/native/ipc.md) · [Native legacy guide](../../../develop/interfaces/skills/native/README.md) · [中文](../../../../../docs_cn/eos/design/runtime/skills/authoring_cn.md)

Built-in C++ is the preferred authoring model. Native ABI v2 remains a legacy migration path for deployment-owned libraries.

Define a `builtin::Definition` with exact identity, parameter and result JSON Schemas, declared primitives/resources/sources/IPC channels/claims, reentrancy, and a `Task<Outcome>` handler. Register definitions explicitly in the island registry; do not use static initialization.

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

Use `Execution` wrappers only: start/wait/cancel primitive operations, resource reads, SIGNALs, parent/child mailbox operations, checkpoints, and diagnostics. Source declarations exist in shared metadata, but built-in `Execution` does not yet provide timestamped source wrappers. A skill must not retain invocation references or access Dispatcher, hardware, model internals, filesystem, network, or workflow state directly. Return stable machine-readable reason codes; messages are bounded operator text and must not be parsed.

Declare the minimum capability set. Validate behavior with fake hooks: schema admission, success/rejection/failure, cancellation and timeout, concurrent operations, same-device ordering, mailbox backpressure and expiry, child cleanup, and logging redaction. Debug with execution snapshots, journal checkpoints, correlation IDs, `journalctl`, and `eos-skill-journal`; never add payload logging to diagnose a failure.

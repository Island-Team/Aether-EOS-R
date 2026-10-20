# Complete minimal native skill

[简体中文](../../../../../../docs_cn/eos/develop/interfaces/skills/native/example_cn.md)

[Authoring overview](README.md) · [ABI contract](abi.md) · [Build and install](build-install-test.md)

The C++17 facade owns descriptor storage, contains exceptions at the C ABI, and completes exactly once from the returned `Outcome`:

```cpp
#include <eos/skill/sdk.hpp>

using eos::skill::Json;

eos::skill::Outcome RunExample(eos::skill::Execution& execution) {
  if (execution.cancelled()) return eos::skill::Outcome::Cancelled();
  (void)execution.logger().info("example_started");
  (void)execution.checkpoint(
      "example.started", eos::skill::CheckpointOptions().Progress(0.0));
  return eos::skill::Outcome::Success(
      "completed", {{"ok", true}}, "example completed successfully");
}

eos::skill::Library BuildLibrary() {
  eos::skill::SkillBuilder skill(
      "example.pure", "1.0.0",
      "Returns a successful result without device effects.",
      Json{{"type", "object"}, {"additionalProperties", false}},
      Json{{"type", "object"},
           {"properties", {{"ok", {{"type", "boolean"}}}}},
           {"required", {"ok"}},
           {"additionalProperties", false}},
      &RunExample);
  return eos::skill::LibraryBuilder("example.skills", "1.0.0", "example-build")
      .Skill(std::move(skill))
      .Build();
}

EOS_SKILL_EXPORT_LIBRARY(BuildLibrary)
```

When a declared capability fails, return a stage-specific diagnostic rather than a generic failure or capability payload:

```cpp
return eos::skill::Outcome::Failure(
    "detection.read_failed", "detection snapshot read failed: capability unavailable");
```

Add `.Primitive`, `.Resource`, `.Signal`, `.Claim`, and `.Reentrant` to the builder as needed. Their declarations remain mandatory capability allowlists. Reason codes are stable machine-readable values; optional outcome messages are bounded, safe operator text and must never contain capability or personal-data payloads. A current host exposes these messages in the execution snapshot and terminal diagnostics, while an older ABI v2 host safely ignores the appended diagnostic extensions. For C authors, the equivalent tables and execute callback are documented in [ABI v2](abi.md); both interfaces export the same query symbol.

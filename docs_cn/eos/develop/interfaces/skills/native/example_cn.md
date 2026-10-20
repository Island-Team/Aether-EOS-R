# 完整的最小原生技能

[English](../../../../../../docs/eos/develop/interfaces/skills/native/example.md)

[编写总览](README_cn.md) · [ABI 契约](abi_cn.md) · [构建与安装](build-install-test_cn.md)

以下 C++17 示例使用 SDK 管理描述符存储，把异常限制在 C ABI 内，并将返回的 `Outcome` 转成恰好一次完成。失败时使用稳定、按阶段区分的 reason 与安全的操作员消息。

以下示例保留可直接使用的代码：

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

```cpp
return eos::skill::Outcome::Failure(
    "detection.read_failed", "detection snapshot read failed: capability unavailable");
```

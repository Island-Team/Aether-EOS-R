#include <eos/skill/sdk.hpp>

namespace {

eos::skill::Outcome Run(eos::skill::Execution& execution) {
  (void)execution.logger().info("standalone.started");
  (void)execution.checkpoint(
      "standalone.completed", eos::skill::CheckpointOptions().Progress(1.0));
  return eos::skill::Outcome::Success("completed", {{"ok", true}});
}

eos::skill::Library BuildLibrary() {
  eos::skill::SkillBuilder skill(
      "standalone", "1.0.0", "Standalone EOS skill package qualification fixture.",
      {{"type", "object"}, {"additionalProperties", false}},
      {{"type", "object"},
       {"properties", {{"ok", {{"type", "boolean"}}}}},
       {"required", {"ok"}},
       {"additionalProperties", false}},
      &Run);
  return eos::skill::LibraryBuilder("test.standalone", "1.0.0", "package-test")
      .Skill(std::move(skill))
      .Build();
}

}  // namespace

EOS_SKILL_EXPORT_LIBRARY(BuildLibrary)

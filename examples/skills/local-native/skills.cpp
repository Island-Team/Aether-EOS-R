#include "share/eos/skills/native/skills.h"

#include "src/eos/runtime/skills/builtin/registry.h"

#include <utility>

namespace island::skills::native {

void RegisterSkills(::eos::runtime::skills::builtin::Registry& registry) {
  namespace builtin = ::eos::runtime::skills::builtin;
  builtin::Definition skill;
  skill.metadata.name = "example.hello";
  skill.metadata.version = "1.0.0";
  skill.metadata.description = "Return a result without accessing hardware.";
  skill.metadata.parameters_schema = {{"type", "object"}, {"additionalProperties", false}};
  skill.metadata.returns_schema = {{"type", "object"},
                                   {"properties", {{"ok", {{"type", "boolean"}}}}},
                                   {"required", {"ok"}},
                                   {"additionalProperties", false}};
  skill.metadata.reentrant = true;
  skill.handler = [](builtin::Execution&) -> builtin::Task<builtin::Outcome> {
    co_return builtin::Outcome::Completed({{"ok", true}});
  };
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native

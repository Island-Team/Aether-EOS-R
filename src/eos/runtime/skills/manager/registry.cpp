#include "src/eos/runtime/skills/manager/registry.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace eos::runtime::skills {

RuntimeRegistry::RuntimeRegistry(std::vector<SkillRuntime> runtimes) {
  if (runtimes.empty()) throw std::invalid_argument("at least one skill runtime is required");
  std::set<std::string> runtime_ids;
  runtimes_.reserve(runtimes.size());
  for (auto& runtime : runtimes) {
    if (runtime.runtime_id.empty() || !runtime_ids.insert(runtime.runtime_id).second)
      throw std::invalid_argument("skill runtime IDs must be unique non-empty strings");
    RuntimeEntry entry;
    entry.runtime_id = std::move(runtime.runtime_id);
    entry.provider = &runtime.provider.get();
    entry.catalog = std::make_unique<Catalog>(*entry.provider);
    runtimes_.push_back(std::move(entry));
  }
  for (const auto& runtime : runtimes_) {
    for (const auto& skill : runtime.catalog->List()) {
      skills_.push_back(skill);
      const auto key = std::make_pair(skill.name, skill.version);
      if (!owners_
               .emplace(key, OwnedSkill{&skills_.back(), runtime.provider, runtime.catalog.get(),
                                        runtime.runtime_id})
               .second)
        throw std::invalid_argument("duplicate admitted skill identity: " + skill.name + "@" +
                                    skill.version);
    }
  }
  // Rebind metadata pointers after aggregate-vector growth has completed.
  for (auto& [identity, owner] : owners_)
    for (const auto& skill : skills_)
      if (skill.name == identity.first && skill.version == identity.second) {
        owner.metadata = &skill;
        break;
      }
}

std::optional<OwnedSkill> RuntimeRegistry::Find(const std::string& name,
                                                const std::string& version) const {
  const auto found = owners_.find({name, version});
  return found == owners_.end() ? std::nullopt : std::optional<OwnedSkill>(found->second);
}

void RuntimeRegistry::ValidateSignal(const OwnedSkill& owner, const SignalDescriptor& descriptor,
                                     const nlohmann::json& payload) const {
  if (owner.catalog == nullptr) throw std::logic_error("skill owner has no runtime catalog");
  owner.catalog->ValidateSignal(descriptor, payload);
}

}  // namespace eos::runtime::skills

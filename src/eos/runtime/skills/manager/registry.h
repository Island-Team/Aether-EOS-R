#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/eos/runtime/skills/core/catalog.h"

namespace eos::runtime::skills {

/// One logical skill runtime supervised by SkillManager.
struct SkillRuntime {
  /// Stable process-local identity used to scope provider-context claims.
  std::string runtime_id;
  /// Provider implementing the skills owned by this runtime.
  std::reference_wrapper<Provider> provider;
};

/// An admitted skill and the logical runtime that exclusively owns it.
struct OwnedSkill {
  /// Immutable aggregate-catalog metadata.
  const Metadata* metadata{nullptr};
  /// Runtime provider used for validation and execution.
  Provider* provider{nullptr};
  /// Runtime-local catalog used for signal validation.
  const Catalog* catalog{nullptr};
  /// Stable owning logical-runtime identity.
  std::string runtime_id;
};

/// Immutable exact-identity routing table for all admitted skill runtimes.
class RuntimeRegistry final {
 public:
  /// Discovers runtimes and rejects duplicate runtime or skill identities.
  explicit RuntimeRegistry(std::vector<SkillRuntime> runtimes);

  /// Returns the aggregate admitted skill catalog.
  const std::vector<Metadata>& List() const noexcept { return skills_; }
  /// Returns the exact owner of `{name, version}`, or no owner.
  std::optional<OwnedSkill> Find(const std::string& name, const std::string& version) const;
  /// Validates a signal payload using its owning runtime catalog.
  void ValidateSignal(const OwnedSkill& owner, const SignalDescriptor& descriptor,
                      const nlohmann::json& payload) const;

 private:
  struct RuntimeEntry {
    std::string runtime_id;
    Provider* provider{nullptr};
    std::unique_ptr<Catalog> catalog;
  };

  std::vector<RuntimeEntry> runtimes_;
  std::vector<Metadata> skills_;
  std::map<std::pair<std::string, std::string>, OwnedSkill> owners_;
};

}  // namespace eos::runtime::skills

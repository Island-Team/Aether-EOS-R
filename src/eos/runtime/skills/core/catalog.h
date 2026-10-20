#pragma once
#include <string>
#include <vector>

#include "src/eos/runtime/skills/core/model.h"
namespace eos::runtime::skills {
class Catalog {
 public:
  explicit Catalog(Provider& provider);
  const std::vector<Metadata>& List() const { return skills_; }
  const Metadata* Find(const std::string& name, const std::string& version) const;
  void ValidateSignal(const SignalDescriptor& descriptor, const nlohmann::json& payload) const;

 private:
  std::vector<Metadata> skills_;
};
std::vector<ResolvedClaim> ResolveClaims(const Metadata&, const nlohmann::json& arguments,
                                         const std::string& provider_context);
}  // namespace eos::runtime::skills

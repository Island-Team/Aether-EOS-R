#include "src/eos/runtime/skills/manager/scheduler.h"
namespace eos::runtime::skills::detail {
bool ClaimsConflict(const std::vector<ResolvedClaim>& left,
                    const std::vector<ResolvedClaim>& right) {
  for (const auto& a : left)
    for (const auto& b : right) {
      if (a.kind == b.kind && a.value == b.value &&
          (a.mode == ClaimMode::kExclusiveUse || b.mode == ClaimMode::kExclusiveUse))
        return true;
    }
  return false;
}
}  // namespace eos::runtime::skills::detail

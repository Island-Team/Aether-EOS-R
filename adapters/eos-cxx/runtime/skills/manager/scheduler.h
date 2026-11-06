#pragma once
#include <vector>

#include "adapters/eos-cxx/runtime/skills/core/model.h"
namespace eos::runtime::skills::detail {
bool ClaimsConflict(const std::vector<ResolvedClaim>& left,
                    const std::vector<ResolvedClaim>& right);
}  // namespace eos::runtime::skills::detail

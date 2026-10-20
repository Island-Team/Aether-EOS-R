#include "src/eos/runtime/northbound/subscribe.h"

#include <cstdint>
#include <utility>

namespace eos::runtime::northbound {
ChangeTracker::ChangeTracker(const nlohmann::json& baseline)
    : canonical_(CanonicalForm(baseline)) {}

std::string ChangeTracker::CanonicalForm(const nlohmann::json& snapshot) {
  nlohmann::json canonical = snapshot;
  canonical.erase("sequence");
  canonical.erase("timestamp");
  return canonical.dump();
}

std::optional<std::uint64_t> ChangeTracker::Observe(const nlohmann::json& snapshot,
                                                    std::chrono::steady_clock::time_point now) {
  std::string canonical = CanonicalForm(snapshot);
  if (canonical == canonical_) {
    return std::nullopt;
  }
  if (now - sent_ < kMinIntervalMs) {
    return std::nullopt;
  }
  canonical_ = std::move(canonical);
  sent_ = now;
  return snapshot.value<std::uint64_t>("sequence", 0);
}

}  // namespace eos::runtime::northbound

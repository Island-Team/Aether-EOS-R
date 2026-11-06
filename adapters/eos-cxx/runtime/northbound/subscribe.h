/// Resource change detection used by the gRPC WatchResource stream.

#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace eos::runtime::northbound {

constexpr std::chrono::milliseconds kMinIntervalMs{100};

/// Tracks one resource snapshot, ignoring projection bookkeeping fields.
class ChangeTracker final {
 public:
  explicit ChangeTracker(const nlohmann::json& baseline);

  /// Returns the observed sequence when content changed outside the throttle.
  std::optional<std::uint64_t> Observe(const nlohmann::json& snapshot,
                                       std::chrono::steady_clock::time_point now);

 private:
  static std::string CanonicalForm(const nlohmann::json& snapshot);

  std::string canonical_;
  std::chrono::steady_clock::time_point sent_{};
};

}  // namespace eos::runtime::northbound

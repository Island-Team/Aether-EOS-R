#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>
#include "src/eos/runtime/skills/core/provider.h"
#include "src/eos/lib/clock.h"
namespace eos::runtime::skills {
class SourcePool final {
 public:
  explicit SourcePool(std::size_t samples_per_topic, const eos::lib::Clock& clock,
                      std::string boot_id);
  void Declare(const SourceDescriptor&);
  SourceSample Publish(const std::string&, std::int64_t, nlohmann::json);
  std::optional<SourceSample> Query(const std::string&, SourceLookup,
                                    std::int64_t timestamp_ns = 0) const;
  std::optional<SourceSample> WaitForNewer(const std::string&, std::uint64_t,
                                           std::chrono::milliseconds,
                                           const std::function<bool()>& cancelled = {}) const;
  void Close();
 private:
  struct Timeline { nlohmann::json schema; std::deque<SourceSample> samples; };
  std::size_t limit_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool closed_{false};
  std::uint64_t sequence_{0};
  const eos::lib::Clock& clock_;
  std::string boot_id_;
  std::unordered_map<std::string, Timeline> topics_;
};
}  // namespace eos::runtime::skills

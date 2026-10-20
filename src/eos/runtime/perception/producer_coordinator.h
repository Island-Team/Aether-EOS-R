#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eos::runtime::perception {

enum class ProducerLifecycle {
  kStopped,
  kStarting,
  kRunning,
  kStopping,
};

struct ProducerStatus {
  ProducerLifecycle lifecycle{ProducerLifecycle::kStopped};
  std::size_t consumer_count{0};
  std::size_t start_attempt_count{0};
  std::size_t start_count{0};
  std::size_t stop_count{0};
};

struct ProducerSource {
  using Stop = std::function<void()>;
  using Factory = std::function<Stop()>;

  std::string name;
  std::vector<std::string> dependencies;
  Factory factory;
};

namespace detail {
struct ProducerCoordinatorState;
}

/// A move-only demand token. Releasing the final token stops the source and
/// any transitive dependencies no longer required by another token.
class ProducerLease final {
 public:
  ProducerLease() = default;
  ~ProducerLease();
  ProducerLease(ProducerLease&& other) noexcept;
  ProducerLease& operator=(ProducerLease&& other) noexcept;
  ProducerLease(const ProducerLease&) = delete;
  ProducerLease& operator=(const ProducerLease&) = delete;

  explicit operator bool() const noexcept { return state_ != nullptr; }
  /// True when this acquisition activated at least one producer in its dependency closure.
  bool activated() const noexcept { return state_ != nullptr && activated_; }
  void Reset() noexcept;

 private:
  friend class ProducerCoordinator;
  ProducerLease(std::shared_ptr<detail::ProducerCoordinatorState> state,
                 std::size_t source, bool activated) noexcept;

  std::shared_ptr<detail::ProducerCoordinatorState> state_;
  std::size_t source_{0};
  bool activated_{false};
};

/// Coordinates demand for a fixed, named, acyclic graph of producers.
class ProducerCoordinator final {
 public:
  explicit ProducerCoordinator(std::vector<ProducerSource> sources);
  ~ProducerCoordinator() = default;
  ProducerCoordinator(const ProducerCoordinator&) = delete;
  ProducerCoordinator& operator=(const ProducerCoordinator&) = delete;
  ProducerCoordinator(ProducerCoordinator&&) noexcept = default;
  ProducerCoordinator& operator=(ProducerCoordinator&&) noexcept = default;

  /// Acquires one consumer of `source`. Factory failures are propagated after
  /// rolling back this acquisition and all producers it started.
  ProducerLease Acquire(std::string_view source) const;
  ProducerStatus Status(std::string_view source) const;

 private:
  std::shared_ptr<detail::ProducerCoordinatorState> state_;
};

}  // namespace eos::runtime::perception

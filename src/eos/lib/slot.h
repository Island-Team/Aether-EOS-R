/// Wait-free SPSC freshness handoff for trivially copyable payloads.
#ifndef SRC_LIB_SLOT_H_
#define SRC_LIB_SLOT_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "src/eos/lib/clock.h"

namespace eos::lib {

/// Returns no value when the latest publish exceeds the freshness budget.
template <typename T>
class Slot {
  static_assert(std::is_trivially_copyable_v<T>,
                "Slot payloads must be trivially copyable — no heap-owned data crosses layers");

 public:
  Slot() = default;
  Slot(const Slot&) = delete;
  Slot& operator=(const Slot&) = delete;

  /// Publishes value with monotonic timestamp t (from the shared Clock).
  /// Producer thread only. Wait-free: two plain stores plus one atomic exchange.
  void Publish(const T& value, MonotonicTime t) {
    Cell& cell = cells_[back_];
    cell.value = value;
    cell.timestamp = t;
    back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndexMask;
  }

  /// Reads the latest published value if fresh against the consumer's budget.
  /// Consumer thread only. Wait-free: one atomic load, at most one exchange,
  /// and one exclusively owned buffer read.
  std::optional<T> Read(MonotonicTime now, std::chrono::nanoseconds fresh_budget) const {
    AcquireLatest();
    if (!has_value_) {
      return std::nullopt;
    }
    const Cell& cell = cells_[front_];
    if (now - cell.timestamp > fresh_budget) {
      return std::nullopt;
    }
    return cell.value;
  }

  /// Latest publish timestamp, or std::nullopt if never published. Consumer
  /// thread only; for staleness reporting and degradation decisions.
  std::optional<MonotonicTime> LastTimestamp() const {
    AcquireLatest();
    if (!has_value_) {
      return std::nullopt;
    }
    return cells_[front_].timestamp;
  }

 private:
  struct Cell {
    T value{};
    MonotonicTime timestamp{};
  };

  static constexpr std::uint32_t kCount = 3;
  static constexpr std::uint32_t kIndexMask = 0x3;
  static constexpr std::uint32_t kDirty = 0x4;

  void AcquireLatest() const {
    if ((middle_.load(std::memory_order_acquire) & kDirty) == 0) {
      return;
    }
    front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndexMask;
    has_value_ = true;
  }

  Cell cells_[kCount]{};
  std::uint32_t back_ = 2;           // Producer-owned.
  mutable std::uint32_t front_ = 0;  // Consumer-owned.
  mutable bool has_value_ = false;   // Consumer-owned.
  mutable std::atomic<std::uint32_t> middle_{1};
};

}  // namespace eos::lib

#endif  // SRC_LIB_SLOT_H_

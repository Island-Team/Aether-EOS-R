/// Time source abstraction over steady/system clocks. Modules take a Clock& at
/// construction; none call std::chrono::*::now() directly, so freshness
/// checks, watchdog deadlines, and snapshot timestamps stay deterministic
/// under tests and replay.
#ifndef SRC_LIB_CLOCK_H_
#define SRC_LIB_CLOCK_H_

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace eos::lib {

/// Monotonic timestamp used in freshness-checked slots and watchdog budgets.
using MonotonicTime = std::chrono::steady_clock::time_point;
/// Wall-clock timestamp for logs, snapshots, and wire payloads.
using WallTime = std::chrono::system_clock::time_point;

/// Clock domain carried by a device or middleware event timestamp. As in ROS,
/// source time is not interchangeable with the steady clock used for deadlines.
enum class SourceClockKind : std::uint8_t {
  kUnknown,
  kUnix,
  kSteadyBoot,
  kRos,
  kSimulation,
  kDevice,
};

struct SourceTime {
  std::int64_t nanoseconds{0};
  SourceClockKind kind{SourceClockKind::kUnknown};
  std::string clock_id;
};

/// Complete timing context for an observation. Receipt time is authoritative
/// for local freshness; wall time is for external correlation only.
struct ObservationTime {
  std::optional<SourceTime> source;
  MonotonicTime received_at;
  WallTime observed_at;
};

/// Interface for thread-safe time reads. Implementations may synchronize.
class Clock {
 public:
  virtual ~Clock() = default;
  virtual MonotonicTime MonotonicNow() const = 0;
  virtual WallTime WallNow() const = 0;
};

/// Production clock over std::chrono steady/system clocks. Reads are RT-safe:
/// they use the platform clocks without allocation or user-space blocking.
class SystemClock final : public Clock {
 public:
  MonotonicTime MonotonicNow() const override { return std::chrono::steady_clock::now(); }
  WallTime WallNow() const override { return std::chrono::system_clock::now(); }
};

/// Deterministic clock for tests and the replay backend. Time advances only
/// via Advance() or Set(); both clocks start at the values given (or epoch).
class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(MonotonicTime mono, WallTime wall = WallTime{}) : mono_(mono), wall_(wall) {}
  ManualClock(const ManualClock&) = delete;
  ManualClock& operator=(const ManualClock&) = delete;
  ManualClock(ManualClock&&) = delete;
  ManualClock& operator=(ManualClock&&) = delete;

  MonotonicTime MonotonicNow() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return mono_;
  }
  WallTime WallNow() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return wall_;
  }

  /// Moves both clocks forward by delta.
  void Advance(std::chrono::nanoseconds delta) {
    if (delta < std::chrono::nanoseconds::zero()) {
      throw std::invalid_argument("ManualClock cannot advance by a negative duration");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    mono_ += std::chrono::duration_cast<MonotonicTime::duration>(delta);
    wall_ += std::chrono::duration_cast<WallTime::duration>(delta);
  }
  void Set(MonotonicTime mono, WallTime wall) {
    std::lock_guard<std::mutex> lock(mutex_);
    mono_ = mono;
    wall_ = wall;
  }

 private:
  mutable std::mutex mutex_;
  MonotonicTime mono_{};
  WallTime wall_{};
};

}  // namespace eos::lib

#endif  // SRC_LIB_CLOCK_H_

/// Bounded timers for EOS control-plane work. Callbacks never execute on the
/// production scheduler thread; use ManualTimerService for deterministic tests.
#ifndef SRC_EOS_LIB_TIMER_H_
#define SRC_EOS_LIB_TIMER_H_

#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "src/eos/lib/clock.h"

namespace eos::lib {

enum class TimerState { kScheduled, kReady, kRunning, kCompleted, kCancelled };

enum class PeriodicPolicy {
  /// Keep the original phase and skip ticks that passed while a callback ran.
  kFixedRateSkipMissed,
  /// Start the next period when the preceding callback finishes.
  kFixedDelay,
};

struct TimerOptions {
  std::size_t callback_workers{1};
  std::size_t max_timers{1024};
  std::size_t ready_capacity{256};
  std::string thread_name{"eos-timer"};
  std::function<void(std::exception_ptr)> exception_handler;
};

namespace timer_detail {
struct TimerControl;
class TimerOwner;
}  // namespace timer_detail

class TimerHandle {
 public:
  TimerHandle() = default;
  ~TimerHandle() noexcept;
  TimerHandle(TimerHandle&& other) noexcept;
  TimerHandle& operator=(TimerHandle&& other) noexcept;
  TimerHandle(const TimerHandle&) = delete;
  TimerHandle& operator=(const TimerHandle&) = delete;

  void Cancel() noexcept;
  void CancelAndWait() noexcept;
  TimerState state() const noexcept;
  explicit operator bool() const noexcept { return control_ != nullptr; }

 private:
  explicit TimerHandle(std::shared_ptr<timer_detail::TimerControl> control);
  std::shared_ptr<timer_detail::TimerControl> control_;

  friend class ThreadTimerService;
  friend class ManualTimerService;
};

class TimerService {
 public:
  using Callback = std::function<void()>;
  virtual ~TimerService() = default;

  virtual const Clock& clock() const noexcept = 0;
  virtual TimerHandle ScheduleAt(MonotonicTime deadline, Callback callback) = 0;
  virtual TimerHandle ScheduleEvery(std::chrono::nanoseconds interval, Callback callback,
                                    PeriodicPolicy policy =
                                        PeriodicPolicy::kFixedRateSkipMissed) = 0;
  virtual void Shutdown() = 0;

  template <class Rep, class Period>
  TimerHandle ScheduleAfter(std::chrono::duration<Rep, Period> delay, Callback callback) {
    const auto duration = std::chrono::duration_cast<MonotonicTime::duration>(delay);
    return ScheduleAt(clock().MonotonicNow() + duration, std::move(callback));
  }
};

class ThreadTimerService final : public TimerService {
 public:
  explicit ThreadTimerService(TimerOptions options = {});
  ThreadTimerService(const Clock& clock, TimerOptions options = {});
  ~ThreadTimerService() override;
  ThreadTimerService(const ThreadTimerService&) = delete;
  ThreadTimerService& operator=(const ThreadTimerService&) = delete;

  const Clock& clock() const noexcept override;
  TimerHandle ScheduleAt(MonotonicTime deadline, Callback callback) override;
  TimerHandle ScheduleEvery(std::chrono::nanoseconds interval, Callback callback,
                            PeriodicPolicy policy =
                                        PeriodicPolicy::kFixedRateSkipMissed) override;
  /// Stops the service and, for callers outside its callback workers, waits for
  /// all callbacks and callback captures to be released. A callback-worker call
  /// only initiates shutdown: it cannot wait for itself, and other callbacks
  /// already running may finish asynchronously.
  void Shutdown() override;

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

class ManualTimerService final : public TimerService {
 public:
  explicit ManualTimerService(TimerOptions options = {},
                              MonotonicTime start = MonotonicTime{},
                              WallTime wall_start = WallTime{});
  ~ManualTimerService() override;
  ManualTimerService(const ManualTimerService&) = delete;
  ManualTimerService& operator=(const ManualTimerService&) = delete;

  const Clock& clock() const noexcept override;
  TimerHandle ScheduleAt(MonotonicTime deadline, Callback callback) override;
  TimerHandle ScheduleEvery(std::chrono::nanoseconds interval, Callback callback,
                            PeriodicPolicy policy =
                                        PeriodicPolicy::kFixedRateSkipMissed) override;
  /// Stops the service and waits for an active Drain to finish. When called by
  /// that Drain's thread, shutdown is initiated and returns without waiting for
  /// the calling callback to unwind.
  void Shutdown() override;

  void AdvanceBy(std::chrono::nanoseconds delta);
  void AdvanceTo(MonotonicTime time);
  void RunUntilIdle();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};

}  // namespace eos::lib

#endif  // SRC_EOS_LIB_TIMER_H_

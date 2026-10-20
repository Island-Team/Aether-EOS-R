#include "src/eos/lib/timer.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace eos::lib {
namespace timer_detail {

struct TimerControl;

class TimerOwner {
 public:
  virtual ~TimerOwner() = default;
  virtual void Cancel(const std::shared_ptr<TimerControl>& control) noexcept = 0;
};

struct TimerControl {
  std::atomic<TimerState> state{TimerState::kScheduled};
  std::weak_ptr<TimerOwner> owner;
  TimerService::Callback callback;
  MonotonicTime deadline;
  std::chrono::nanoseconds interval{0};
  PeriodicPolicy policy{PeriodicPolicy::kFixedRateSkipMissed};
  bool periodic{false};
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  std::size_t executions{0};
  std::thread::id running_thread;
};

struct Entry {
  MonotonicTime deadline;
  std::uint64_t sequence;
  std::shared_ptr<TimerControl> control;
};

struct EntryLater {
  bool operator()(const Entry& left, const Entry& right) const {
    if (left.deadline != right.deadline) return left.deadline > right.deadline;
    return left.sequence > right.sequence;
  }
};

class EntryQueue {
 public:
  explicit EntryQueue(std::size_t capacity) { entries_.reserve(capacity); }

  bool empty() const noexcept { return entries_.empty(); }
  std::size_t size() const noexcept { return entries_.size(); }
  const Entry& top() const noexcept { return entries_.front(); }

  void push(Entry entry) noexcept {
    entries_.push_back(std::move(entry));
    std::push_heap(entries_.begin(), entries_.end(), EntryLater{});
  }

  Entry pop() noexcept {
    std::pop_heap(entries_.begin(), entries_.end(), EntryLater{});
    Entry entry = std::move(entries_.back());
    entries_.pop_back();
    return entry;
  }

  void Remove(const std::shared_ptr<TimerControl>& control) noexcept {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) { return entry.control == control; }),
                   entries_.end());
    std::make_heap(entries_.begin(), entries_.end(), EntryLater{});
  }

 private:
  std::vector<Entry> entries_;
};

void ReportException(const TimerOptions& options, std::exception_ptr error) noexcept {
  try {
    if (options.exception_handler) {
      options.exception_handler(error);
      return;
    }
    try {
      std::rethrow_exception(error);
    } catch (const std::exception& exception) {
      std::cerr << "timer callback threw: " << exception.what() << '\n';
    } catch (...) {
      std::cerr << "timer callback threw a non-standard exception\n";
    }
  } catch (...) {
    // Exception reporting must not terminate a worker.
  }
}

void SetThreadName(const std::string& name) {
#if defined(__linux__)
  if (!name.empty()) pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#else
  (void)name;
#endif
}

void MarkExecuting(const std::shared_ptr<TimerControl>& control) {
  std::lock_guard<std::mutex> lock(control->wait_mutex);
  control->running_thread = std::this_thread::get_id();
  ++control->executions;
}

void MarkStopped(const std::shared_ptr<TimerControl>& control) {
  {
    std::lock_guard<std::mutex> lock(control->wait_mutex);
    --control->executions;
    control->running_thread = {};
  }
  control->wait_cv.notify_all();
}

MonotonicTime NextDeadline(const TimerControl& control, MonotonicTime now) {
  if (control.policy == PeriodicPolicy::kFixedDelay) return now + control.interval;
  auto next = control.deadline + control.interval;
  if (next < now) {
    next += control.interval * ((now - next) / control.interval);
    if (next < now) next += control.interval;
  }
  return next;
}

void ValidateOptions(const TimerOptions& options, bool threaded) {
  if (options.max_timers == 0 || options.ready_capacity == 0 ||
      (threaded && options.callback_workers == 0)) {
    throw std::invalid_argument("timer capacities and worker count must be non-zero");
  }
}

thread_local const void* current_timer_worker = nullptr;

}  // namespace timer_detail

using timer_detail::Entry;
using timer_detail::EntryQueue;
using timer_detail::TimerControl;

TimerHandle::TimerHandle(std::shared_ptr<TimerControl> control) : control_(std::move(control)) {}

TimerHandle::~TimerHandle() noexcept { Cancel(); }

TimerHandle::TimerHandle(TimerHandle&& other) noexcept : control_(std::move(other.control_)) {}

TimerHandle& TimerHandle::operator=(TimerHandle&& other) noexcept {
  if (this != &other) {
    Cancel();
    control_ = std::move(other.control_);
  }
  return *this;
}

void TimerHandle::Cancel() noexcept {
  if (!control_) return;
  if (auto owner = control_->owner.lock()) owner->Cancel(control_);
}

void TimerHandle::CancelAndWait() noexcept {
  if (!control_) return;
  Cancel();
  std::unique_lock<std::mutex> lock(control_->wait_mutex);
  if (control_->running_thread == std::this_thread::get_id()) return;
  control_->wait_cv.wait(lock, [this] {
    return control_->executions == 0;
  });
}

TimerState TimerHandle::state() const noexcept {
  return control_ ? control_->state.load(std::memory_order_acquire) : TimerState::kCancelled;
}

class ThreadTimerService::Impl final : public timer_detail::TimerOwner,
                                        public std::enable_shared_from_this<Impl> {
 public:
  explicit Impl(TimerOptions options)
      : owned_clock_(std::make_unique<SystemClock>()), clock_(owned_clock_.get()),
        options_(std::move(options)), heap_(options_.max_timers) {
    timer_detail::ValidateOptions(options_, true);
    ready_.reserve(options_.ready_capacity);
  }

  Impl(const Clock& clock, TimerOptions options)
      : clock_(&clock), options_(std::move(options)), heap_(options_.max_timers) {
    timer_detail::ValidateOptions(options_, true);
    ready_.reserve(options_.ready_capacity);
  }

  ~Impl() override { DetachRemainingThreads(); }

  void Start() {
    auto self = shared_from_this();
    workers_.reserve(options_.callback_workers);
    {
      std::lock_guard<std::mutex> lock(threads_mutex_);
      ++live_threads_;
    }
    try {
      scheduler_ = std::thread([self] {
        self->SchedulerLoop();
        self->ThreadExited();
      });
    } catch (...) {
      ThreadExited();
      throw;
    }
    try {
      for (std::size_t i = 0; i < options_.callback_workers; ++i) {
        {
          std::lock_guard<std::mutex> lock(threads_mutex_);
          ++live_threads_;
        }
        try {
          workers_.emplace_back([self, i] {
            self->WorkerLoop(i);
            self->ThreadExited();
          });
        } catch (...) {
          ThreadExited();
          throw;
        }
      }
    } catch (...) {
      Shutdown();
      throw;
    }
  }

  TimerHandle Add(MonotonicTime deadline, TimerService::Callback callback,
                  std::chrono::nanoseconds interval, PeriodicPolicy policy) {
    if (!callback) return {};
    auto control = std::make_shared<TimerControl>();
    control->owner = shared_from_this();
    control->callback = std::move(callback);
    control->deadline = deadline;
    control->interval = interval;
    control->policy = policy;
    control->periodic = interval.count() > 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_ || active_ >= options_.max_timers) return {};
      ++active_;
      heap_.push(Entry{deadline, sequence_++, control});
    }
    scheduler_cv_.notify_one();
    return TimerHandle(std::move(control));
  }

  void Cancel(const std::shared_ptr<TimerControl>& control) noexcept override {
    TimerService::Callback retired_callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const TimerState state = control->state.load(std::memory_order_relaxed);
      if (state == TimerState::kCompleted || state == TimerState::kCancelled) return;
      control->state.store(TimerState::kCancelled, std::memory_order_release);
      if (state != TimerState::kRunning) {
        --active_;
        RemoveQueued(control);
        retired_callback = std::move(control->callback);
      }
      scheduler_cv_.notify_all();
      worker_cv_.notify_all();
      control->wait_cv.notify_all();
    }
  }

  void Shutdown() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!shutdown_) {
      shutdown_ = true;
      shutdown_initializing_ = true;
      shutdown_initializer_ = std::this_thread::get_id();
      scheduler_cv_.notify_all();
      worker_cv_.notify_all();
      while (!heap_.empty()) {
        auto control = heap_.pop().control;
        auto retired_callback = CancelForShutdown(control);
        lock.unlock();
        retired_callback = {};
        lock.lock();
      }
      while (!ready_.empty()) {
        auto control = std::move(ready_.back());
        ready_.pop_back();
        auto retired_callback = CancelForShutdown(control);
        lock.unlock();
        retired_callback = {};
        lock.lock();
      }
      shutdown_initializing_ = false;
      shutdown_initializer_ = {};
      shutdown_cv_.notify_all();
    } else if (shutdown_initializing_) {
      if (shutdown_initializer_ == std::this_thread::get_id()) return;
      shutdown_cv_.wait(lock, [this] { return !shutdown_initializing_; });
    }
    lock.unlock();
    scheduler_cv_.notify_all();
    worker_cv_.notify_all();
    FinishThreads();
  }

  const Clock& clock() const noexcept { return *clock_; }

 private:
  void RemoveQueued(const std::shared_ptr<TimerControl>& control) {
    if (control->state.load(std::memory_order_relaxed) != TimerState::kCancelled) return;
    heap_.Remove(control);
    ready_.erase(std::remove(ready_.begin(), ready_.end(), control), ready_.end());
  }

  TimerService::Callback CancelForShutdown(const std::shared_ptr<TimerControl>& control) {
    const TimerState state = control->state.load(std::memory_order_relaxed);
    if (state == TimerState::kScheduled || state == TimerState::kReady) {
      control->state.store(TimerState::kCancelled, std::memory_order_release);
      --active_;
      control->wait_cv.notify_all();
      return std::move(control->callback);
    }
    return {};
  }

  void SchedulerLoop() {
    timer_detail::SetThreadName(options_.thread_name);
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutdown_) {
      while (!heap_.empty() &&
             heap_.top().control->state.load(std::memory_order_relaxed) !=
                 TimerState::kScheduled) {
        heap_.pop();
      }
      if (heap_.empty()) {
        scheduler_cv_.wait(lock, [this] { return shutdown_ || !heap_.empty(); });
        continue;
      }
      const MonotonicTime deadline = heap_.top().deadline;
       if (clock_->MonotonicNow() < deadline) {
        scheduler_cv_.wait_until(lock, deadline);
        continue;
      }
      if (ready_.size() >= options_.ready_capacity) {
        scheduler_cv_.wait(lock, [this] {
          return shutdown_ || ready_.size() < options_.ready_capacity;
        });
        continue;
      }
      auto control = heap_.pop().control;
      control->state.store(TimerState::kReady, std::memory_order_release);
      ready_.push_back(std::move(control));
      worker_cv_.notify_one();
    }
  }

  void WorkerLoop(std::size_t index) {
    struct WorkerScope {
      explicit WorkerScope(const void* owner) { timer_detail::current_timer_worker = owner; }
      ~WorkerScope() { timer_detail::current_timer_worker = nullptr; }
    } worker_scope(this);
    timer_detail::SetThreadName(options_.thread_name + "-" + std::to_string(index));
    for (;;) {
      std::shared_ptr<TimerControl> control;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        worker_cv_.wait(lock, [this] { return shutdown_ || !ready_.empty(); });
        if (shutdown_) return;
        control = std::move(ready_.front());
        ready_.erase(ready_.begin());
        scheduler_cv_.notify_one();
        if (control->state.load(std::memory_order_relaxed) != TimerState::kReady) continue;
        control->state.store(TimerState::kRunning, std::memory_order_release);
        timer_detail::MarkExecuting(control);
      }

      bool threw = false;
      try {
        control->callback();
      } catch (...) {
        threw = true;
        timer_detail::ReportException(options_, std::current_exception());
      }

      timer_detail::MarkStopped(control);

      TimerService::Callback retired_callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool cancelled = control->state.load(std::memory_order_relaxed) ==
                                TimerState::kCancelled;
        if (!cancelled && control->periodic && !threw && !shutdown_) {
          control->deadline = timer_detail::NextDeadline(*control, clock_->MonotonicNow());
          control->state.store(TimerState::kScheduled, std::memory_order_release);
          heap_.push(Entry{control->deadline, sequence_++, control});
          scheduler_cv_.notify_one();
        } else {
          control->state.store((threw && control->periodic) || cancelled ||
                                       (control->periodic && shutdown_)
                                    ? TimerState::kCancelled
                                    : TimerState::kCompleted,
                               std::memory_order_release);
          --active_;
          retired_callback = std::move(control->callback);
        }
      }
    }
  }

  void DetachRemainingThreads() noexcept {
    std::lock_guard<std::mutex> lock(threads_mutex_);
    try {
      if (scheduler_.joinable()) scheduler_.detach();
      for (auto& worker : workers_) {
        if (worker.joinable()) worker.detach();
      }
    } catch (...) {
      // Valid joinable threads are detachable; retain noexcept as a last resort.
    }
  }

  void FinishThreads() noexcept {
    if (timer_detail::current_timer_worker == this) {
      DetachRemainingThreads();
      return;
    }

    for (;;) {
      std::thread thread;
      {
        std::unique_lock<std::mutex> lock(threads_mutex_);
        if (scheduler_.joinable()) {
          thread = std::move(scheduler_);
        } else {
          const auto worker = std::find_if(workers_.begin(), workers_.end(),
                                           [](const std::thread& value) {
                                             return value.joinable();
                                           });
          if (worker == workers_.end()) {
            threads_cv_.wait(lock, [this] {
              return threads_in_flight_ == 0 && live_threads_ == 0;
            });
            return;
          }
          thread = std::move(*worker);
        }
        ++threads_in_flight_;
      }

      try {
        thread.join();
      } catch (...) {
        try {
          if (thread.joinable()) thread.detach();
        } catch (...) {
        }
      }
      {
        std::lock_guard<std::mutex> lock(threads_mutex_);
        --threads_in_flight_;
      }
      threads_cv_.notify_all();
    }
  }

  void ThreadExited() noexcept {
    {
      std::lock_guard<std::mutex> lock(threads_mutex_);
      --live_threads_;
    }
    threads_cv_.notify_all();
  }

  std::unique_ptr<SystemClock> owned_clock_;
  const Clock* clock_;
  TimerOptions options_;
  std::mutex mutex_;
  std::condition_variable scheduler_cv_;
  std::condition_variable worker_cv_;
  std::condition_variable shutdown_cv_;
  EntryQueue heap_;
  std::vector<std::shared_ptr<TimerControl>> ready_;
  std::uint64_t sequence_{0};
  std::size_t active_{0};
  bool shutdown_{false};
  bool shutdown_initializing_{false};
  std::thread::id shutdown_initializer_;
  std::mutex threads_mutex_;
  std::condition_variable threads_cv_;
  std::size_t threads_in_flight_{0};
  std::size_t live_threads_{0};
  std::thread scheduler_;
  std::vector<std::thread> workers_;
};

ThreadTimerService::ThreadTimerService(TimerOptions options)
    : impl_(std::make_shared<Impl>(std::move(options))) {
  impl_->Start();
}

ThreadTimerService::ThreadTimerService(const Clock& clock, TimerOptions options)
    : impl_(std::make_shared<Impl>(clock, std::move(options))) {
  impl_->Start();
}

ThreadTimerService::~ThreadTimerService() { Shutdown(); }

const Clock& ThreadTimerService::clock() const noexcept { return impl_->clock(); }

TimerHandle ThreadTimerService::ScheduleAt(MonotonicTime deadline, Callback callback) {
  return impl_->Add(deadline, std::move(callback), std::chrono::nanoseconds::zero(),
                    PeriodicPolicy::kFixedRateSkipMissed);
}

TimerHandle ThreadTimerService::ScheduleEvery(std::chrono::nanoseconds interval,
                                              Callback callback, PeriodicPolicy policy) {
  if (interval <= std::chrono::nanoseconds::zero()) return {};
  return impl_->Add(clock().MonotonicNow() + interval, std::move(callback), interval, policy);
}

void ThreadTimerService::Shutdown() {
  auto impl = impl_;
  if (impl) impl->Shutdown();
}

class ManualTimerService::Impl final : public timer_detail::TimerOwner,
                                       public std::enable_shared_from_this<Impl> {
 public:
  Impl(TimerOptions options, MonotonicTime start, WallTime wall_start)
      : options_(std::move(options)), clock_(start, wall_start), heap_(options_.max_timers) {
    timer_detail::ValidateOptions(options_, false);
  }

  TimerHandle Add(MonotonicTime deadline, TimerService::Callback callback,
                  std::chrono::nanoseconds interval, PeriodicPolicy policy) {
    if (!callback) return {};
    auto control = std::make_shared<TimerControl>();
    control->owner = shared_from_this();
    control->callback = std::move(callback);
    control->deadline = deadline;
    control->interval = interval;
    control->policy = policy;
    control->periodic = interval.count() > 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_ || active_ >= options_.max_timers) return {};
      ++active_;
      heap_.push(Entry{deadline, sequence_++, control});
    }
    return TimerHandle(std::move(control));
  }

  void Cancel(const std::shared_ptr<TimerControl>& control) noexcept override {
    TimerService::Callback retired_callback;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const TimerState state = control->state.load(std::memory_order_relaxed);
      if (state == TimerState::kCompleted || state == TimerState::kCancelled) return;
      control->state.store(TimerState::kCancelled, std::memory_order_release);
      if (state != TimerState::kRunning) {
        --active_;
        heap_.Remove(control);
        retired_callback = std::move(control->callback);
      }
      control->wait_cv.notify_all();
    }
  }

  void RunUntilIdle() {
    EnterRun();
    Drain();
    ExitRun();
  }

  void Shutdown() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!shutdown_) {
      shutdown_ = true;
      shutdown_initializing_ = true;
      shutdown_initializer_ = std::this_thread::get_id();
      while (!heap_.empty()) {
        auto control = heap_.pop().control;
        TimerService::Callback retired_callback;
        if (control->state.load(std::memory_order_relaxed) == TimerState::kScheduled) {
          control->state.store(TimerState::kCancelled, std::memory_order_release);
          --active_;
          control->wait_cv.notify_all();
          retired_callback = std::move(control->callback);
        }
        lock.unlock();
        retired_callback = {};
        lock.lock();
      }
      shutdown_initializing_ = false;
      shutdown_initializer_ = {};
      shutdown_cv_.notify_all();
    } else if (shutdown_initializing_) {
      if (shutdown_initializer_ == std::this_thread::get_id()) return;
      shutdown_cv_.wait(lock, [this] { return !shutdown_initializing_; });
    }

    if (run_thread_ == std::this_thread::get_id()) return;
    run_cv_.wait(lock, [this] { return run_depth_ == 0; });
  }

  void AdvanceBy(std::chrono::nanoseconds delta) {
    EnterRun();
    clock_.Advance(delta);
    Drain();
    ExitRun();
  }

  void AdvanceTo(MonotonicTime time) {
    EnterRun();
    const auto now = clock_.MonotonicNow();
    if (time < now) {
      ExitRun();
      throw std::invalid_argument("manual timer cannot move backwards");
    }
    clock_.Advance(std::chrono::duration_cast<std::chrono::nanoseconds>(time - now));
    Drain();
    ExitRun();
  }

  TimerOptions options_;
  ManualClock clock_;

 private:
  void EnterRun() {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto current = std::this_thread::get_id();
    run_cv_.wait(lock, [&] { return run_depth_ == 0 || run_thread_ == current; });
    run_thread_ = current;
    ++run_depth_;
  }

  void ExitRun() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (--run_depth_ == 0) run_thread_ = {};
    }
    run_cv_.notify_all();
  }

  void Drain() {
    for (;;) {
      std::shared_ptr<TimerControl> control;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_ || heap_.empty() || heap_.top().deadline > clock_.MonotonicNow()) return;
        control = heap_.pop().control;
        if (control->state.load(std::memory_order_relaxed) != TimerState::kScheduled) continue;
        control->state.store(TimerState::kRunning, std::memory_order_release);
        timer_detail::MarkExecuting(control);
      }

      bool threw = false;
      try {
        control->callback();
      } catch (...) {
        threw = true;
        timer_detail::ReportException(options_, std::current_exception());
      }

      timer_detail::MarkStopped(control);
      TimerService::Callback retired_callback;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool cancelled = control->state.load(std::memory_order_relaxed) ==
                               TimerState::kCancelled;
        if (!cancelled && control->periodic && !threw && !shutdown_) {
          control->deadline = timer_detail::NextDeadline(*control, clock_.MonotonicNow());
          control->state.store(TimerState::kScheduled, std::memory_order_release);
          heap_.push(Entry{control->deadline, sequence_++, control});
        } else {
          control->state.store((threw && control->periodic) || cancelled ||
                                       (control->periodic && shutdown_)
                                   ? TimerState::kCancelled
                                   : TimerState::kCompleted,
                               std::memory_order_release);
          --active_;
          retired_callback = std::move(control->callback);
        }
      }
    }
  }
  std::mutex mutex_;
  std::condition_variable run_cv_;
  std::condition_variable shutdown_cv_;
  std::thread::id run_thread_;
  std::size_t run_depth_{0};
  EntryQueue heap_;
  std::uint64_t sequence_{0};
  std::size_t active_{0};
  bool shutdown_{false};
  bool shutdown_initializing_{false};
  std::thread::id shutdown_initializer_;
};

ManualTimerService::ManualTimerService(TimerOptions options, MonotonicTime start,
                                       WallTime wall_start)
    : impl_(std::make_shared<Impl>(std::move(options), start, wall_start)) {}

ManualTimerService::~ManualTimerService() { Shutdown(); }

const Clock& ManualTimerService::clock() const noexcept { return impl_->clock_; }

TimerHandle ManualTimerService::ScheduleAt(MonotonicTime deadline, Callback callback) {
  return impl_->Add(deadline, std::move(callback), std::chrono::nanoseconds::zero(),
                    PeriodicPolicy::kFixedRateSkipMissed);
}

TimerHandle ManualTimerService::ScheduleEvery(std::chrono::nanoseconds interval,
                                              Callback callback, PeriodicPolicy policy) {
  if (interval <= std::chrono::nanoseconds::zero()) return {};
  return impl_->Add(clock().MonotonicNow() + interval, std::move(callback), interval, policy);
}

void ManualTimerService::Shutdown() {
  auto impl = impl_;
  if (impl) impl->Shutdown();
}

void ManualTimerService::AdvanceBy(std::chrono::nanoseconds delta) {
  if (delta < std::chrono::nanoseconds::zero()) {
    throw std::invalid_argument("manual timer cannot move backwards");
  }
  auto impl = impl_;
  impl->AdvanceBy(delta);
}

void ManualTimerService::AdvanceTo(MonotonicTime time) {
  auto impl = impl_;
  impl->AdvanceTo(time);
}

void ManualTimerService::RunUntilIdle() {
  auto impl = impl_;
  impl->RunUntilIdle();
}

}  // namespace eos::lib

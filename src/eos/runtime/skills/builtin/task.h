#pragma once

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

#include "src/eos/lib/cancellation.h"
#include "src/eos/lib/timer.h"

namespace eos::runtime::skills::builtin {

class SuspendFor {
 public:
  SuspendFor(eos::lib::TimerService& timers, eos::lib::CancellationToken cancellation,
             std::chrono::milliseconds duration) noexcept
      : timers_(&timers), cancellation_token_(std::move(cancellation)), duration_(duration) {}
  SuspendFor(SuspendFor&& other) noexcept
      : timers_(other.timers_), cancellation_token_(std::move(other.cancellation_token_)),
        duration_(other.duration_), state_(std::move(other.state_)),
        timer_(std::move(other.timer_)), cancellation_(std::move(other.cancellation_)) {}
  SuspendFor& operator=(SuspendFor&& other) noexcept {
    if (this != &other) {
      CancelAndWait();
      timers_ = other.timers_;
      cancellation_token_ = std::move(other.cancellation_token_);
      duration_ = other.duration_;
      state_ = std::move(other.state_);
      timer_ = std::move(other.timer_);
      cancellation_ = std::move(other.cancellation_);
    }
    return *this;
  }
  SuspendFor(const SuspendFor&) = delete;
  SuspendFor& operator=(const SuspendFor&) = delete;
  ~SuspendFor() { CancelAndWait(); }

  bool await_ready() const noexcept {
    return duration_.count() <= 0 || cancellation_token_.IsCancelled();
  }
  bool await_suspend(std::coroutine_handle<> continuation) {
    state_ = std::make_shared<State>();
    state_->continuation = continuation;
    auto timer = timers_->ScheduleAfter(duration_, [state = state_] { Complete(state); });
    if (!timer) throw std::runtime_error("built-in skill timer scheduling rejected");
    timer_ = std::move(timer);
    cancellation_ = cancellation_token_.Register([state = state_] { Complete(state); });

    std::lock_guard lock(state_->mutex);
    if (state_->phase == State::Phase::kCompleted) return false;
    state_->phase = State::Phase::kSuspended;
    return true;
  }
  void await_resume() const noexcept {}

 private:
  struct State {
    enum class Phase { kRegistering, kSuspended, kCompleted };
    std::mutex mutex;
    Phase phase{Phase::kRegistering};
    std::coroutine_handle<> continuation;
  };

  static void Complete(const std::shared_ptr<State>& state) {
    std::coroutine_handle<> continuation;
    {
      std::lock_guard lock(state->mutex);
      if (state->phase == State::Phase::kCompleted) return;
      if (state->phase == State::Phase::kRegistering) {
        state->phase = State::Phase::kCompleted;
        return;
      }
      state->phase = State::Phase::kCompleted;
      continuation = state->continuation;
    }
    continuation.resume();
  }

  void CancelAndWait() noexcept {
    cancellation_.Reset();
    timer_.CancelAndWait();
  }

  eos::lib::TimerService* timers_;
  eos::lib::CancellationToken cancellation_token_;
  std::chrono::milliseconds duration_;
  std::shared_ptr<State> state_;
  eos::lib::TimerHandle timer_;
  eos::lib::Registration cancellation_;
};

// Lazy coroutine task. Awaited capabilities may resume it on any runtime-owned
// executor; Get() blocks only the Provider compatibility call at the boundary.
template <typename T>
class Task {
 public:
  struct promise_type;
  using Handle = std::coroutine_handle<promise_type>;

  explicit Task(Handle handle) : handle_(handle) {}
  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() {
    if (handle_) handle_.destroy();
  }

  T Get() {
    if (!handle_) throw std::logic_error("empty built-in skill task");
    auto& promise = handle_.promise();
    if (!handle_.done()) handle_.resume();
    std::unique_lock lock(promise.mutex);
    promise.finished.wait(lock, [&] { return promise.done; });
    if (promise.error) std::rethrow_exception(promise.error);
    return std::move(promise.value.value());
  }

  using Completion = std::function<void(std::optional<T>, std::exception_ptr)>;
  class Started {
   public:
    explicit Started(Handle handle) : handle_(handle) {}
    ~Started() {
      if (started_) Wait();
      handle_.destroy();
    }
    void Resume() {
      started_ = true;
      handle_.resume();
    }
    bool Done() const {
      std::lock_guard lock(handle_.promise().mutex);
      return handle_.promise().done;
    }
    void Wait() const {
      std::unique_lock lock(handle_.promise().mutex);
      handle_.promise().finished.wait(lock, [&] { return handle_.promise().done; });
    }
   private:
    Handle handle_;
    bool started_{false};
  };

  std::shared_ptr<Started> Start(Completion completion) && {
    if (!handle_) throw std::logic_error("empty built-in skill task");
    handle_.promise().completion = std::move(completion);
    Handle started = std::exchange(handle_, {});
    return std::make_shared<Started>(started);
  }

  struct Awaiter {
    Handle handle;
    bool await_ready() const noexcept { return handle.done(); }
    void await_suspend(std::coroutine_handle<> continuation) {
      handle.promise().continuation = continuation;
      handle.resume();
    }
    T await_resume() {
      if (handle.promise().error) std::rethrow_exception(handle.promise().error);
      return std::move(handle.promise().value.value());
    }
  };
  Awaiter operator co_await() & { return Awaiter{handle_}; }

  struct promise_type {
    std::optional<T> value;
    std::exception_ptr error;
    std::coroutine_handle<> continuation;
    std::mutex mutex;
    std::condition_variable finished;
    bool done{false};
    Completion completion;

    Task get_return_object() { return Task(Handle::from_promise(*this)); }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(Handle handle) const noexcept {
        auto& promise = handle.promise();
        const std::coroutine_handle<> continuation =
            promise.continuation ? promise.continuation : std::noop_coroutine();
        if (promise.completion) {
          try {
            promise.completion(std::move(promise.value), promise.error);
          } catch (...) {
          }
        }
        {
          std::lock_guard lock(promise.mutex);
          promise.done = true;
        }
        promise.finished.notify_all();
        return continuation;
      }
      void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }
    void unhandled_exception() { error = std::current_exception(); }
    template <typename U>
    void return_value(U&& result) { value.emplace(std::forward<U>(result)); }
  };

 private:
  Handle handle_;
};

}  // namespace eos::runtime::skills::builtin

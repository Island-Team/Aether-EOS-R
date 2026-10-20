#include "src/eos/lib/cancellation.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace eos::lib {
namespace cancellation_internal {

struct Callback {
  explicit Callback(std::function<void()> callback) : function(std::move(callback)) {}

  std::mutex mutex;
  std::condition_variable finished;
  std::function<void()> function;
  bool active{true};
  bool executing{false};
  std::thread::id executing_thread;
};

struct State {
  std::atomic<bool> cancelled{false};
  std::mutex mutex;
  std::unordered_map<Callback*, std::shared_ptr<Callback>> callbacks;
};

namespace {

void Invoke(const std::shared_ptr<Callback>& callback) noexcept {
  std::function<void()> function;
  {
    std::lock_guard<std::mutex> lock(callback->mutex);
    if (!callback->active) return;
    callback->active = false;
    callback->executing = true;
    callback->executing_thread = std::this_thread::get_id();
    function = std::move(callback->function);
  }

  try {
    if (function) function();
  } catch (...) {
    // Cancellation is best-effort notification; one observer must not prevent
    // the remaining observers or unwind through Cancel/Register.
  }
  function = nullptr;

  {
    std::lock_guard<std::mutex> lock(callback->mutex);
    callback->executing = false;
    callback->executing_thread = {};
  }
  callback->finished.notify_all();
}

}  // namespace
}  // namespace cancellation_internal

Registration::Registration(std::shared_ptr<cancellation_internal::State> state,
                           std::shared_ptr<cancellation_internal::Callback> callback)
    : state_(std::move(state)), callback_(std::move(callback)) {}

Registration::~Registration() { Reset(); }

Registration::Registration(Registration&& other) noexcept
    : state_(std::move(other.state_)), callback_(std::move(other.callback_)) {}

Registration& Registration::operator=(Registration&& other) noexcept {
  if (this == &other) return *this;
  Reset();
  state_ = std::move(other.state_);
  callback_ = std::move(other.callback_);
  return *this;
}

void Registration::Reset() noexcept {
  std::shared_ptr<cancellation_internal::State> state = std::move(state_);
  std::shared_ptr<cancellation_internal::Callback> callback = std::move(callback_);
  if (!callback) return;

  if (state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto it = state->callbacks.find(callback.get());
    if (it != state->callbacks.end() && it->second == callback) state->callbacks.erase(it);
  }

  std::function<void()> discarded;
  std::unique_lock<std::mutex> lock(callback->mutex);
  callback->active = false;
  discarded = std::move(callback->function);
  if (callback->executing && callback->executing_thread != std::this_thread::get_id()) {
    callback->finished.wait(lock, [&] { return !callback->executing; });
  }
}

bool CancellationToken::IsCancelled() const noexcept {
  return state_ && state_->cancelled.load(std::memory_order_acquire);
}

Registration CancellationToken::Register(std::function<void()> function) const {
  if (!state_) return {};

  std::shared_ptr<cancellation_internal::Callback> callback;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->cancelled.load(std::memory_order_relaxed)) {
      callback = std::make_shared<cancellation_internal::Callback>(std::move(function));
      state_->callbacks.emplace(callback.get(), callback);
      return Registration(state_, std::move(callback));
    }
  }

  try {
    if (function) function();
  } catch (...) {
    // Match Cancel(): callback failures are contained at the notification boundary.
  }
  return {};
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<cancellation_internal::State>()) {}

bool CancellationSource::IsCancelled() const noexcept {
  return state_ && state_->cancelled.load(std::memory_order_acquire);
}

void CancellationSource::Cancel() noexcept {
  if (!state_) return;

  std::unordered_map<cancellation_internal::Callback*,
                     std::shared_ptr<cancellation_internal::Callback>> callbacks;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) return;
    callbacks.swap(state_->callbacks);
  }

  for (const auto& entry : callbacks) cancellation_internal::Invoke(entry.second);
}

}  // namespace eos::lib

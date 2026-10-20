/// Cooperative cancellation shared by asynchronous and blocking operations.
/// Cancellation is a one-way state transition. Callbacks run synchronously on
/// the cancelling or registering thread and must therefore remain short.
#ifndef SRC_LIB_CANCELLATION_H_
#define SRC_LIB_CANCELLATION_H_

#include <functional>
#include <memory>
#include <utility>

namespace eos::lib {

namespace cancellation_internal {
struct Callback;
struct State;
}  // namespace cancellation_internal

/// RAII ownership of one cancellation callback. Destroying or resetting the
/// registration prevents a callback that has not started and waits for an
/// in-flight callback, except when called by that callback itself.
class Registration {
 public:
  Registration() = default;
  ~Registration();
  Registration(Registration&& other) noexcept;
  Registration& operator=(Registration&& other) noexcept;
  Registration(const Registration&) = delete;
  Registration& operator=(const Registration&) = delete;

  void Reset() noexcept;
  explicit operator bool() const noexcept { return callback_ != nullptr; }

 private:
  friend class CancellationToken;
  Registration(std::shared_ptr<cancellation_internal::State> state,
               std::shared_ptr<cancellation_internal::Callback> callback);

  std::shared_ptr<cancellation_internal::State> state_;
  std::shared_ptr<cancellation_internal::Callback> callback_;
};

/// Copyable observation handle for a CancellationSource.
class CancellationToken {
 public:
  CancellationToken() = default;

  bool IsCancelled() const noexcept;

  /// Registers a callback, or invokes it before returning if cancellation has
  /// already occurred. Exceptions from callbacks never escape this function.
  Registration Register(std::function<void()> callback) const;

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<cancellation_internal::State> state)
      : state_(std::move(state)) {}

  std::shared_ptr<cancellation_internal::State> state_;
};

/// Owner of a shared cancellation state. Cancel() is thread-safe and
/// idempotent; the first caller synchronously invokes all current callbacks.
class CancellationSource {
 public:
  CancellationSource();
  CancellationSource(CancellationSource&&) noexcept = default;
  CancellationSource& operator=(CancellationSource&&) noexcept = default;
  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;

  CancellationToken token() const noexcept { return CancellationToken(state_); }
  bool IsCancelled() const noexcept;
  void Cancel() noexcept;

 private:
  std::shared_ptr<cancellation_internal::State> state_;
};

}  // namespace eos::lib

#endif  // SRC_LIB_CANCELLATION_H_

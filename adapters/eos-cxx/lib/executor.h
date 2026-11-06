/// Bounded executors for asynchronous and same-thread work dispatch.
#ifndef SRC_EOS_LIB_EXECUTOR_H_
#define SRC_EOS_LIB_EXECUTOR_H_

#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace eos::lib {

enum class PostStatus {
  kAccepted,
  kFull,
  kStopping,
};

enum class ShutdownMode {
  kDrain,
  kCancelPending,
};

using ExecutorExceptionHandler = std::function<void(std::exception_ptr)>;

class Executor {
 public:
  virtual ~Executor() = default;

  /// Posts a non-empty task. Empty tasks are rejected with invalid_argument.
  virtual PostStatus Post(std::function<void()> task) = 0;
};

struct ThreadPoolOptions {
  ThreadPoolOptions() = default;
  ThreadPoolOptions(std::size_t workers, std::size_t capacity, std::string name = {})
      : worker_count(workers), queue_capacity(capacity), thread_name(std::move(name)) {}

  std::size_t worker_count = 1;
  std::size_t queue_capacity = 1;
  std::string thread_name;
  ExecutorExceptionHandler exception_handler;
};

/// Bounded FIFO pool; destruction from a worker cancels pending work without joining itself.
class ThreadPoolExecutor final : public Executor {
 public:
  explicit ThreadPoolExecutor(ThreadPoolOptions options);
  ~ThreadPoolExecutor() override;

  ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
  ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;
  ThreadPoolExecutor(ThreadPoolExecutor&&) = delete;
  ThreadPoolExecutor& operator=(ThreadPoolExecutor&&) = delete;

  PostStatus Post(std::function<void()> task) override;

  /// Starts shutdown. The first call selects the shutdown mode; later calls
  /// are no-ops. CancelPending discards queued tasks but does not interrupt
  /// tasks that are already running.
  void Shutdown(ShutdownMode mode = ShutdownMode::kDrain);

  /// Starts drain shutdown and waits for all workers. Calling this from this
  /// pool's worker throws logic_error without starting shutdown, since a worker
  /// cannot join itself.
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Runs accepted work synchronously on the calling thread.
class InlineExecutor final : public Executor {
 public:
  explicit InlineExecutor(ExecutorExceptionHandler exception_handler = {});

  PostStatus Post(std::function<void()> task) override;

 private:
  ExecutorExceptionHandler exception_handler_;
};

}  // namespace eos::lib

#endif  // SRC_EOS_LIB_EXECUTOR_H_

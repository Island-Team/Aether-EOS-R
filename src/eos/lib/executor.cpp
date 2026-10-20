#include "src/eos/lib/executor.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

namespace eos::lib {
namespace {

thread_local const void* current_pool = nullptr;

void ReportException(const ExecutorExceptionHandler& handler, std::exception_ptr error) noexcept {
  if (!handler) return;
  try {
    handler(std::move(error));
  } catch (...) {
  }
}

void SetCurrentThreadName(const std::string& requested_name) noexcept {
#ifdef __linux__
  if (requested_name.empty()) return;
  // Linux limits names to 15 visible bytes plus the terminating null.
  const std::string name = requested_name.substr(0, 15);
  (void)pthread_setname_np(pthread_self(), name.c_str());
#else
  (void)requested_name;
#endif
}

}  // namespace

struct ThreadPoolExecutor::Impl {
  struct State {
    explicit State(ThreadPoolOptions options)
        : capacity(options.queue_capacity),
          thread_name(std::move(options.thread_name)),
          exception_handler(std::move(options.exception_handler)) {}

    const std::size_t capacity;
    const std::string thread_name;
    const ExecutorExceptionHandler exception_handler;
    std::mutex mutex;
    std::condition_variable work_available;
    std::deque<std::function<void()>> queue;
    bool stopping = false;
    ShutdownMode shutdown_mode = ShutdownMode::kDrain;
  };

  explicit Impl(ThreadPoolOptions options) : state(std::make_shared<State>(options)) {
    workers.reserve(options.worker_count);
    try {
      for (std::size_t i = 0; i < options.worker_count; ++i) {
        workers.emplace_back([shared = state] {
          current_pool = shared.get();
          SetCurrentThreadName(shared->thread_name);
          for (;;) {
            std::function<void()> task;
            {
              std::unique_lock<std::mutex> lock(shared->mutex);
              shared->work_available.wait(lock,
                                          [&] { return shared->stopping || !shared->queue.empty(); });
              if (shared->stopping &&
                  (shared->shutdown_mode == ShutdownMode::kCancelPending ||
                   shared->queue.empty())) {
                break;
              }
              task = std::move(shared->queue.front());
              shared->queue.pop_front();
            }
            try {
              task();
            } catch (...) {
              ReportException(shared->exception_handler, std::current_exception());
            }
          }
          current_pool = nullptr;
        });
      }
    } catch (...) {
      Shutdown(ShutdownMode::kCancelPending);
      JoinWorkers();
      throw;
    }
  }

  PostStatus Post(std::function<void()> task) {
    if (!task) throw std::invalid_argument("executor task must not be empty");
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stopping) return PostStatus::kStopping;
    if (state->queue.size() >= state->capacity) return PostStatus::kFull;
    state->queue.push_back(std::move(task));
    state->work_available.notify_one();
    return PostStatus::kAccepted;
  }

  void Shutdown(ShutdownMode mode) {
    std::deque<std::function<void()>> cancelled;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->stopping) return;
      state->stopping = true;
      state->shutdown_mode = mode;
      if (mode == ShutdownMode::kCancelPending) cancelled.swap(state->queue);
    }
    state->work_available.notify_all();
  }

  void CancelPendingForWorkerDestruction() {
    std::deque<std::function<void()>> cancelled;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stopping = true;
      state->shutdown_mode = ShutdownMode::kCancelPending;
      cancelled.swap(state->queue);
    }
    state->work_available.notify_all();
  }

  void JoinWorkers() {
    std::lock_guard<std::mutex> lock(join_mutex);
    for (std::thread& worker : workers) {
      if (!worker.joinable()) continue;
      worker.join();
    }
  }

  void DetachWorkers() {
    std::lock_guard<std::mutex> lock(join_mutex);
    for (std::thread& worker : workers) {
      if (worker.joinable()) worker.detach();
    }
  }

  std::shared_ptr<State> state;
  std::vector<std::thread> workers;
  std::mutex join_mutex;
};

ThreadPoolExecutor::ThreadPoolExecutor(ThreadPoolOptions options) {
  if (options.worker_count == 0) {
    throw std::invalid_argument("executor worker_count must be positive");
  }
  if (options.queue_capacity == 0) {
    throw std::invalid_argument("executor queue_capacity must be positive");
  }
  if (options.thread_name.find('\0') != std::string::npos) {
    throw std::invalid_argument("executor thread_name must not contain null bytes");
  }
  impl_ = std::make_unique<Impl>(std::move(options));
}

ThreadPoolExecutor::~ThreadPoolExecutor() {
  const bool called_from_worker = current_pool == impl_->state.get();
  if (called_from_worker) {
    impl_->CancelPendingForWorkerDestruction();
    impl_->DetachWorkers();
  } else {
    impl_->Shutdown(ShutdownMode::kDrain);
    impl_->JoinWorkers();
  }
}

PostStatus ThreadPoolExecutor::Post(std::function<void()> task) {
  return impl_->Post(std::move(task));
}

void ThreadPoolExecutor::Shutdown(ShutdownMode mode) { impl_->Shutdown(mode); }

void ThreadPoolExecutor::Join() {
  if (current_pool == impl_->state.get()) {
    throw std::logic_error("ThreadPoolExecutor::Join called from its worker");
  }
  impl_->Shutdown(ShutdownMode::kDrain);
  impl_->JoinWorkers();
}

InlineExecutor::InlineExecutor(ExecutorExceptionHandler exception_handler)
    : exception_handler_(std::move(exception_handler)) {}

PostStatus InlineExecutor::Post(std::function<void()> task) {
  if (!task) throw std::invalid_argument("executor task must not be empty");
  try {
    task();
  } catch (...) {
    ReportException(exception_handler_, std::current_exception());
  }
  return PostStatus::kAccepted;
}

}  // namespace eos::lib

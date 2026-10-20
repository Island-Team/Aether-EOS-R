#include "src/eos/lib/executor.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

constexpr auto kWaitTimeout = std::chrono::seconds(2);

class Gate {
 public:
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [&] { return open_; });
  }

  void WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return entered_; });
  }

  void Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool open_ = false;
};

class DestructionNotifier {
 public:
  explicit DestructionNotifier(std::shared_ptr<std::promise<void>> destroyed)
      : destroyed_(std::move(destroyed)) {}

  ~DestructionNotifier() { destroyed_->set_value(); }

 private:
  std::shared_ptr<std::promise<void>> destroyed_;
};

TEST(ExecutorTest, RejectsInvalidConfigurationAndEmptyTasks) {
  EXPECT_THROW(ThreadPoolExecutor(ThreadPoolOptions{0, 1}), std::invalid_argument);
  EXPECT_THROW(ThreadPoolExecutor(ThreadPoolOptions{1, 0}), std::invalid_argument);
  EXPECT_THROW(ThreadPoolExecutor(ThreadPoolOptions{1, 1, std::string("bad\0name", 8)}),
               std::invalid_argument);

  InlineExecutor inline_executor;
  EXPECT_THROW(inline_executor.Post({}), std::invalid_argument);
  ThreadPoolExecutor pool(ThreadPoolOptions{1, 1});
  EXPECT_THROW(pool.Post({}), std::invalid_argument);
}

TEST(ExecutorTest, QueueIsBoundedAndRunsInFifoOrder) {
  ThreadPoolExecutor pool(ThreadPoolOptions{1, 2});
  Gate worker;
  std::mutex mutex;
  std::vector<int> order;

  ASSERT_EQ(pool.Post([&] { worker.Wait(); }), PostStatus::kAccepted);
  worker.WaitUntilEntered();
  ASSERT_EQ(pool.Post([&] {
              std::lock_guard<std::mutex> lock(mutex);
              order.push_back(1);
            }),
            PostStatus::kAccepted);
  ASSERT_EQ(pool.Post([&] {
              std::lock_guard<std::mutex> lock(mutex);
              order.push_back(2);
            }),
            PostStatus::kAccepted);
  EXPECT_EQ(pool.Post([] {}), PostStatus::kFull);

  pool.Shutdown(ShutdownMode::kDrain);
  EXPECT_EQ(pool.Post([] {}), PostStatus::kStopping);
  worker.Open();
  pool.Join();
  EXPECT_EQ(order, (std::vector<int>{1, 2}));
  pool.Shutdown(ShutdownMode::kCancelPending);
  pool.Join();
}

TEST(ExecutorTest, CancelPendingDoesNotInterruptRunningTask) {
  ThreadPoolExecutor pool(ThreadPoolOptions{1, 2});
  Gate running;
  bool completed = false;
  bool pending_ran = false;
  ASSERT_EQ(pool.Post([&] {
              running.Wait();
              completed = true;
            }),
            PostStatus::kAccepted);
  running.WaitUntilEntered();
  ASSERT_EQ(pool.Post([&] { pending_ran = true; }), PostStatus::kAccepted);

  pool.Shutdown(ShutdownMode::kCancelPending);
  running.Open();
  pool.Join();
  EXPECT_TRUE(completed);
  EXPECT_FALSE(pending_ran);
}

TEST(ExecutorTest, JoinInitiatesDrainShutdown) {
  ThreadPoolExecutor pool(ThreadPoolOptions{1, 2});
  std::atomic<int> tasks_run = 0;
  ASSERT_EQ(pool.Post([&] { ++tasks_run; }), PostStatus::kAccepted);
  ASSERT_EQ(pool.Post([&] { ++tasks_run; }), PostStatus::kAccepted);

  auto joined = std::async(std::launch::async, [&] { pool.Join(); });
  const std::future_status status = joined.wait_for(kWaitTimeout);
  if (status != std::future_status::ready) pool.Shutdown();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_EQ(joined.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_EQ(tasks_run.load(), 2);
  EXPECT_EQ(pool.Post([] {}), PostStatus::kStopping);
}

TEST(ExecutorTest, TaskAndHandlerExceptionsDoNotStopWorkers) {
  std::mutex mutex;
  std::condition_variable changed;
  int handled = 0;
  bool final_task_ran = false;
  ThreadPoolOptions options{1, 3};
  options.exception_handler = [&](std::exception_ptr error) {
    EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++handled;
    }
    changed.notify_all();
    throw std::logic_error("handler failure");
  };
  ThreadPoolExecutor pool(std::move(options));
  ASSERT_EQ(pool.Post([] { throw std::runtime_error("task failure"); }), PostStatus::kAccepted);
  ASSERT_EQ(pool.Post([&] {
              {
                std::lock_guard<std::mutex> lock(mutex);
                final_task_ran = true;
              }
              changed.notify_all();
            }),
            PostStatus::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(changed.wait_for(lock, kWaitTimeout,
                                 [&] { return handled == 1 && final_task_ran; }));
  }
  pool.Shutdown();
  pool.Join();
}

TEST(ExecutorTest, JoinFromWorkerIsRejectedWithoutDeadlock) {
  ThreadPoolExecutor pool(ThreadPoolOptions{1, 1});
  std::mutex mutex;
  std::condition_variable changed;
  bool rejected = false;
  ASSERT_EQ(pool.Post([&] {
              try {
                pool.Join();
              } catch (const std::logic_error&) {
                std::lock_guard<std::mutex> lock(mutex);
                rejected = true;
              }
              changed.notify_all();
            }),
            PostStatus::kAccepted);
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(changed.wait_for(lock, kWaitTimeout, [&] { return rejected; }));
  }
  pool.Shutdown();
  pool.Join();
}

TEST(ExecutorTest, DestructionFromWorkerCancelsPendingAndDetachesRunningWorkers) {
  auto pool = std::make_unique<ThreadPoolExecutor>(ThreadPoolOptions{2, 2});
  Gate active_worker;
  Gate destroyer;
  Gate destruction_returned;
  std::atomic<bool> active_completed = false;
  std::atomic<bool> pending_ran = false;
  std::atomic<bool> destruction_completed = false;
  std::atomic<bool> active_observed_destruction_return = false;
  auto active_done = std::make_shared<std::promise<void>>();
  std::future<void> active_done_future = active_done->get_future();
  auto destroyer_done = std::make_shared<std::promise<void>>();
  std::future<void> destroyer_done_future = destroyer_done->get_future();

  ASSERT_EQ(pool->Post([&] {
              active_worker.Wait();
              destruction_returned.Wait();
              active_observed_destruction_return = destruction_completed.load();
              active_completed = true;
              active_done->set_value();
            }),
            PostStatus::kAccepted);
  active_worker.WaitUntilEntered();
  ASSERT_EQ(pool->Post([&] {
              destroyer.Wait();
              pool->Shutdown(ShutdownMode::kDrain);
              pool.reset();
              destruction_completed = true;
              destruction_returned.Open();
              destroyer_done->set_value();
            }),
            PostStatus::kAccepted);
  destroyer.WaitUntilEntered();

  auto pending_destroyed = std::make_shared<std::promise<void>>();
  std::future<void> pending_destroyed_future = pending_destroyed->get_future();
  auto notifier = std::make_shared<DestructionNotifier>(pending_destroyed);
  ASSERT_EQ(pool->Post([notifier, &pending_ran] { pending_ran = true; }),
            PostStatus::kAccepted);
  notifier.reset();

  destroyer.Open();
  active_worker.Open();
  const std::future_status destruction_status = destroyer_done_future.wait_for(kWaitTimeout);
  if (destruction_status != std::future_status::ready) destruction_returned.Open();

  EXPECT_EQ(destruction_status, std::future_status::ready);
  EXPECT_EQ(destroyer_done_future.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_EQ(pending_destroyed_future.wait_for(kWaitTimeout), std::future_status::ready);
  EXPECT_EQ(active_done_future.wait_for(kWaitTimeout), std::future_status::ready);

  EXPECT_TRUE(active_completed);
  EXPECT_TRUE(active_observed_destruction_return);
  EXPECT_FALSE(pending_ran);
}

TEST(ExecutorTest, InlineExecutorContainsTaskAndHandlerExceptions) {
  int handled = 0;
  InlineExecutor executor([&](std::exception_ptr error) {
    ++handled;
    EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
    throw std::logic_error("handler failure");
  });
  EXPECT_EQ(executor.Post([] { throw std::runtime_error("task failure"); }),
            PostStatus::kAccepted);
  EXPECT_EQ(handled, 1);
}

}  // namespace
}  // namespace eos::lib

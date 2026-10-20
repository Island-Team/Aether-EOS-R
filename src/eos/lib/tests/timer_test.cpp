#include "src/eos/lib/timer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {
using namespace std::chrono_literals;

static_assert(std::is_nothrow_destructible_v<TimerHandle>);
static_assert(std::is_nothrow_move_assignable_v<TimerHandle>);
static_assert(noexcept(std::declval<TimerHandle&>().Cancel()));
static_assert(noexcept(std::declval<TimerHandle&>().CancelAndWait()));

TEST(TimerTest, ManualTimersRunInDeadlineThenAdmissionOrder) {
  ManualTimerService timers;
  std::vector<int> calls;
  auto third = timers.ScheduleAfter(20ms, [&] { calls.push_back(3); });
  auto first = timers.ScheduleAfter(10ms, [&] { calls.push_back(1); });
  auto second = timers.ScheduleAfter(10ms, [&] { calls.push_back(2); });

  timers.AdvanceBy(9ms);
  EXPECT_TRUE(calls.empty());
  timers.AdvanceBy(1ms);
  EXPECT_EQ(calls, (std::vector<int>{1, 2}));
  timers.AdvanceBy(10ms);
  EXPECT_EQ(calls, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(first.state(), TimerState::kCompleted);
  EXPECT_EQ(second.state(), TimerState::kCompleted);
  EXPECT_EQ(third.state(), TimerState::kCompleted);
}

TEST(TimerTest, HandleDestructionCancelsTimer) {
  ManualTimerService timers;
  bool called = false;
  {
    auto handle = timers.ScheduleAfter(1ms, [&] { called = true; });
  }
  timers.AdvanceBy(1ms);
  EXPECT_FALSE(called);
}

TEST(TimerTest, AdmissionIsBoundedAndReopensAfterCompletion) {
  TimerOptions options;
  options.max_timers = 1;
  ManualTimerService timers(options);
  auto first = timers.ScheduleAfter(1ms, [] {});
  auto rejected = timers.ScheduleAfter(1ms, [] {});
  EXPECT_TRUE(first);
  EXPECT_FALSE(rejected);

  timers.AdvanceBy(1ms);
  auto admitted = timers.ScheduleAfter(1ms, [] {});
  EXPECT_TRUE(admitted);
}

TEST(TimerTest, FixedRateSkipsMissedTicksAndPreservesPhase) {
  ManualTimerService timers;
  int calls = 0;
  auto handle = timers.ScheduleEvery(10ms, [&] {
    ++calls;
    if (calls == 1) timers.AdvanceBy(25ms);
  });

  timers.AdvanceBy(10ms);  // Callback finishes at t=35; next phase is t=40.
  EXPECT_EQ(calls, 1);
  timers.AdvanceBy(4ms);
  EXPECT_EQ(calls, 1);
  timers.AdvanceBy(1ms);
  EXPECT_EQ(calls, 2);
}

TEST(TimerTest, FixedDelayStartsPeriodAtCallbackCompletion) {
  ManualTimerService timers;
  int calls = 0;
  auto handle = timers.ScheduleEvery(
      10ms,
      [&] {
        ++calls;
        if (calls == 1) timers.AdvanceBy(25ms);
      },
      PeriodicPolicy::kFixedDelay);

  timers.AdvanceBy(10ms);  // Callback finishes at t=35; next run is t=45.
  timers.AdvanceBy(9ms);
  EXPECT_EQ(calls, 1);
  timers.AdvanceBy(1ms);
  EXPECT_EQ(calls, 2);
}

TEST(TimerTest, PeriodicExceptionIsReportedAndCancelsTimer) {
  TimerOptions options;
  int reports = 0;
  options.exception_handler = [&](std::exception_ptr error) {
    ++reports;
    EXPECT_THROW(std::rethrow_exception(error), std::runtime_error);
  };
  ManualTimerService timers(options);
  auto handle = timers.ScheduleEvery(1ms, [] { throw std::runtime_error("failure"); });

  timers.AdvanceBy(1ms);
  EXPECT_EQ(reports, 1);
  EXPECT_EQ(handle.state(), TimerState::kCancelled);
  timers.AdvanceBy(10ms);
  EXPECT_EQ(reports, 1);
}

TEST(TimerTest, ShutdownCancelsManualTimersAndRejectsNewOnes) {
  ManualTimerService timers;
  auto handle = timers.ScheduleAfter(1ms, [] {});
  timers.Shutdown();
  EXPECT_EQ(handle.state(), TimerState::kCancelled);
  EXPECT_FALSE(timers.ScheduleAfter(1ms, [] {}));
}

TEST(TimerTest, ManualShutdownWaitsForActiveCallback) {
  ManualTimerService timers;
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  bool shutdown_returned = false;
  bool second_shutdown_returned = false;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak_capture = capture;
  auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&, capture] {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
  });
  capture.reset();

  std::thread runner([&] { timers.RunUntilIdle(); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  std::thread shutdown([&] {
    timers.Shutdown();
    {
      std::lock_guard<std::mutex> lock(mutex);
      shutdown_returned = true;
    }
    cv.notify_all();
  });
  std::thread second_shutdown([&] {
    timers.Shutdown();
    {
      std::lock_guard<std::mutex> lock(mutex);
      second_shutdown_returned = true;
    }
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_FALSE(cv.wait_for(lock, 50ms, [&] {
      return shutdown_returned || second_shutdown_returned;
    }));
    release = true;
  }
  cv.notify_all();
  runner.join();
  shutdown.join();
  second_shutdown.join();

  EXPECT_TRUE(shutdown_returned);
  EXPECT_TRUE(second_shutdown_returned);
  EXPECT_TRUE(weak_capture.expired());
  EXPECT_EQ(handle.state(), TimerState::kCompleted);
}

TEST(TimerTest, ManualCallbackCanInitiateShutdownWithoutWaitingForItself) {
  ManualTimerService timers;
  bool shutdown_returned = false;
  auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    timers.Shutdown();
    shutdown_returned = true;
  });

  timers.RunUntilIdle();
  EXPECT_TRUE(shutdown_returned);
  EXPECT_EQ(handle.state(), TimerState::kCompleted);
}

TEST(TimerTest, ManualCallbacksCanReenterService) {
  ManualTimerService timers;
  int calls = 0;
  auto outer = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    ++calls;
    auto nested = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] { ++calls; });
    ASSERT_TRUE(nested);
    timers.RunUntilIdle();
  });

  timers.RunUntilIdle();
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(outer.state(), TimerState::kCompleted);
}

TEST(TimerTest, ManualSchedulingIsThreadSafe) {
  TimerOptions options;
  options.max_timers = 256;
  ManualTimerService timers(options);
  std::atomic<int> calls{0};
  std::mutex handles_mutex;
  std::vector<TimerHandle> handles;
  handles.reserve(200);
  std::vector<std::thread> producers;

  for (int producer = 0; producer < 4; ++producer) {
    producers.emplace_back([&] {
      for (int i = 0; i < 50; ++i) {
        auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] { ++calls; });
        ASSERT_TRUE(handle);
        std::lock_guard<std::mutex> lock(handles_mutex);
        handles.push_back(std::move(handle));
      }
    });
  }
  for (auto& producer : producers) producer.join();

  std::thread first_runner([&] { timers.RunUntilIdle(); });
  std::thread second_runner([&] { timers.RunUntilIdle(); });
  first_runner.join();
  second_runner.join();
  EXPECT_EQ(calls.load(), 200);
}

TEST(TimerTest, ThreadServiceRunsCallbacksOnWorkerAndCompletes) {
  TimerOptions options;
  options.callback_workers = 1;
  ThreadTimerService timers(options);
  const std::thread::id caller = std::this_thread::get_id();
  std::mutex mutex;
  std::condition_variable cv;
  bool complete = false;
  std::thread::id callback_thread;
  auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      callback_thread = std::this_thread::get_id();
      complete = true;
    }
    cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return complete; }));
  lock.unlock();
  timers.Shutdown();
  EXPECT_NE(callback_thread, caller);
  EXPECT_EQ(handle.state(), TimerState::kCompleted);
}

TEST(TimerTest, ThreadShutdownDoesNotStartQueuedCallback) {
  TimerOptions options;
  options.callback_workers = 1;
  options.ready_capacity = 1;
  ThreadTimerService timers(options);
  std::mutex mutex;
  std::condition_variable entered_cv;
  std::condition_variable release_cv;
  bool entered = false;
  bool release = false;
  std::atomic<bool> queued_called{false};
  auto running = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    entered_cv.notify_one();
    release_cv.wait(lock, [&] { return release; });
  });
  auto queued = timers.ScheduleAt(timers.clock().MonotonicNow(),
                                  [&] { queued_called.store(true); });

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(entered_cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  std::thread releaser([&] {
    while (queued.state() != TimerState::kCancelled) std::this_thread::yield();
    {
      std::lock_guard<std::mutex> lock(mutex);
      release = true;
    }
    release_cv.notify_one();
  });
  timers.Shutdown();
  releaser.join();
  EXPECT_FALSE(queued_called.load());
  EXPECT_EQ(queued.state(), TimerState::kCancelled);
}

TEST(TimerTest, CallbackCanShutdownThreadService) {
  TimerOptions options;
  options.callback_workers = 2;
  ThreadTimerService timers(options);
  std::mutex mutex;
  std::condition_variable cv;
  bool returned = false;
  auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    timers.Shutdown();
    {
      std::lock_guard<std::mutex> lock(mutex);
      returned = true;
    }
    cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return returned; }));
  lock.unlock();
  handle.CancelAndWait();
  EXPECT_TRUE(handle.state() == TimerState::kCompleted ||
              handle.state() == TimerState::kCancelled);
}

TEST(TimerTest, CallbackShutdownMayLeaveAnotherRunningCallbackAsynchronous) {
  TimerOptions options;
  options.callback_workers = 2;
  ThreadTimerService timers(options);
  std::mutex mutex;
  std::condition_variable cv;
  bool blocker_entered = false;
  bool release_blocker = false;
  bool shutdown_returned = false;
  auto blocker = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    std::unique_lock<std::mutex> lock(mutex);
    blocker_entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release_blocker; });
  });
  auto shutdown = timers.ScheduleAt(timers.clock().MonotonicNow(), [&] {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [&] { return blocker_entered; });
    lock.unlock();
    timers.Shutdown();
    lock.lock();
    shutdown_returned = true;
    cv.notify_all();
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return shutdown_returned; }));
    EXPECT_FALSE(release_blocker);
    release_blocker = true;
  }
  cv.notify_all();

  // A process-owned caller still upgrades initiated shutdown to full quiescence.
  timers.Shutdown();
  blocker.CancelAndWait();
  shutdown.CancelAndWait();
}

TEST(TimerTest, CallbackCanDestroyThreadService) {
  TimerOptions options;
  options.callback_workers = 2;
  auto timers = std::make_unique<ThreadTimerService>(options);
  std::mutex mutex;
  std::condition_variable cv;
  bool destroyed = false;
  auto handle = timers->ScheduleAt(timers->clock().MonotonicNow(), [&] {
    timers.reset();
    {
      std::lock_guard<std::mutex> lock(mutex);
      destroyed = true;
    }
    cv.notify_one();
  });

  std::unique_lock<std::mutex> lock(mutex);
  ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return destroyed; }));
  lock.unlock();
  handle.CancelAndWait();
  EXPECT_TRUE(handle.state() == TimerState::kCompleted ||
              handle.state() == TimerState::kCancelled);
}

TEST(TimerTest, ShutdownReleasesPendingCallbackCaptures) {
  ThreadTimerService timers;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak_capture = capture;
  auto handle = timers.ScheduleAfter(1h, [capture] {});
  capture.reset();

  timers.Shutdown();
  EXPECT_TRUE(weak_capture.expired());
  EXPECT_EQ(handle.state(), TimerState::kCancelled);
}

TEST(TimerTest, ConcurrentShutdownWaitsForCaptureRelease) {
  struct Capture {
    ThreadTimerService* timers;
    std::mutex* mutex;
    std::condition_variable* cv;
    bool* destroying;
    bool* release;

    ~Capture() {
      // Callback-capture destruction may itself reenter shutdown.
      timers->Shutdown();
      std::unique_lock<std::mutex> lock(*mutex);
      *destroying = true;
      cv->notify_all();
      cv->wait(lock, [&] { return *release; });
    }
  };

  ThreadTimerService timers;
  std::mutex mutex;
  std::condition_variable cv;
  bool destroying = false;
  bool release = false;
  bool second_started = false;
  bool second_returned = false;
  auto capture = std::make_shared<Capture>();
  capture->timers = &timers;
  capture->mutex = &mutex;
  capture->cv = &cv;
  capture->destroying = &destroying;
  capture->release = &release;
  auto handle = timers.ScheduleAfter(1h, [capture] {});
  capture.reset();

  std::thread first([&] { timers.Shutdown(); });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return destroying; }));
  }
  std::thread second([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      second_started = true;
    }
    cv.notify_all();
    timers.Shutdown();
    {
      std::lock_guard<std::mutex> lock(mutex);
      second_returned = true;
    }
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return second_started; }));
    EXPECT_FALSE(cv.wait_for(lock, 50ms, [&] { return second_returned; }));
    release = true;
  }
  cv.notify_all();
  first.join();
  second.join();

  EXPECT_TRUE(second_returned);
  EXPECT_EQ(handle.state(), TimerState::kCancelled);
}

TEST(TimerTest, ExternalThreadShutdownWaitsForRunningCallbackAndCaptureRelease) {
  TimerOptions options;
  options.callback_workers = 1;
  ThreadTimerService timers(options);
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  bool shutdown_returned = false;
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> weak_capture = capture;
  auto handle = timers.ScheduleAt(timers.clock().MonotonicNow(), [&, capture] {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
  });
  capture.reset();
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  std::thread shutdown([&] {
    timers.Shutdown();
    {
      std::lock_guard<std::mutex> lock(mutex);
      shutdown_returned = true;
    }
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_FALSE(cv.wait_for(lock, 50ms, [&] { return shutdown_returned; }));
    release = true;
  }
  cv.notify_all();
  shutdown.join();

  EXPECT_TRUE(shutdown_returned);
  EXPECT_TRUE(weak_capture.expired());
  EXPECT_TRUE(handle.state() == TimerState::kCompleted ||
              handle.state() == TimerState::kCancelled);
}

TEST(TimerTest, CancelAndWaitBlocksForPeriodicInvocationWithMultipleWorkers) {
  TimerOptions options;
  options.callback_workers = 4;
  ThreadTimerService timers(options);
  std::mutex mutex;
  std::condition_variable entered_cv;
  std::condition_variable release_cv;
  bool entered = false;
  bool release = false;
  std::atomic<int> calls{0};
  auto handle = timers.ScheduleEvery(1ns, [&] {
    ++calls;
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    entered_cv.notify_one();
    release_cv.wait(lock, [&] { return release; });
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(entered_cv.wait_for(lock, 2s, [&] { return entered; }));
  }
  handle.Cancel();
  std::atomic<bool> wait_returned{false};
  std::thread waiter([&] {
    handle.CancelAndWait();
    wait_returned.store(true, std::memory_order_release);
  });
  EXPECT_FALSE(wait_returned.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  release_cv.notify_one();
  waiter.join();

  EXPECT_TRUE(wait_returned.load(std::memory_order_acquire));
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(handle.state(), TimerState::kCancelled);
}

}  // namespace
}  // namespace eos::lib

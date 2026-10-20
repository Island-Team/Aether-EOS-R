#include "src/eos/lib/cancellation.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

static_assert(!std::is_copy_constructible_v<Registration>);
static_assert(!std::is_copy_assignable_v<Registration>);
static_assert(std::is_nothrow_move_constructible_v<Registration>);
static_assert(std::is_nothrow_move_assignable_v<Registration>);

TEST(CancellationTest, CancelIsIdempotentAndNotifiesEachRegistrationOnce) {
  CancellationSource source;
  std::atomic<int> calls{0};
  auto first = source.token().Register([&] { ++calls; });
  auto second = source.token().Register([&] { ++calls; });

  source.Cancel();
  source.Cancel();

  EXPECT_TRUE(source.IsCancelled());
  EXPECT_TRUE(source.token().IsCancelled());
  EXPECT_EQ(calls.load(), 2);
}

TEST(CancellationTest, RegisterAfterCancellationInvokesImmediatelyAndContainsExceptions) {
  CancellationSource source;
  source.Cancel();
  int calls = 0;

  Registration registration = source.token().Register([&] {
    ++calls;
    throw std::runtime_error("callback failure");
  });

  EXPECT_EQ(calls, 1);
  EXPECT_FALSE(registration);
}

TEST(CancellationTest, DestroyedRegistrationIsNotInvoked) {
  CancellationSource source;
  int calls = 0;
  {
    auto registration = source.token().Register([&] { ++calls; });
  }

  source.Cancel();
  EXPECT_EQ(calls, 0);
}

TEST(CancellationTest, ThrowingCallbackDoesNotPreventOtherCallbacks) {
  CancellationSource source;
  int calls = 0;
  auto throwing = source.token().Register([] { throw std::runtime_error("failure"); });
  auto succeeding = source.token().Register([&] { ++calls; });

  EXPECT_NO_THROW(source.Cancel());
  EXPECT_EQ(calls, 1);
}

TEST(CancellationTest, CallbackCanResetItsOwnRegistration) {
  CancellationSource source;
  Registration registration;
  registration = source.token().Register([&] { registration.Reset(); });

  EXPECT_NO_THROW(source.Cancel());
  EXPECT_FALSE(registration);
}

TEST(CancellationTest, ResetDuringExecutionWaitsForRunningCallback) {
  CancellationSource source;
  std::atomic<bool> entered{false};
  std::atomic<bool> reset_started{false};
  std::atomic<int> callback_calls{0};
  std::atomic<bool> reset_finished{false};
  Registration registration = source.token().Register([&] {
    entered.store(true, std::memory_order_release);
    while (!reset_started.load(std::memory_order_acquire)) std::this_thread::yield();
    callback_calls.fetch_add(1, std::memory_order_release);
  });

  std::thread canceller([&] { source.Cancel(); });
  while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
  std::thread resetter([&] {
    reset_started.store(true, std::memory_order_release);
    registration.Reset();
    reset_finished.store(true, std::memory_order_release);
  });

  resetter.join();
  canceller.join();
  EXPECT_EQ(callback_calls.load(std::memory_order_acquire), 1);
  EXPECT_TRUE(reset_finished.load(std::memory_order_acquire));
  EXPECT_FALSE(registration);
}

TEST(CancellationTest, ConcurrentRegistrationAndCancellationAreSafe) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    CancellationSource source;
    std::atomic<int> calls{0};
    Registration registration;
    std::thread registrar([&] {
      registration = source.token().Register([&] { ++calls; });
    });
    std::thread canceller([&] { source.Cancel(); });
    registrar.join();
    canceller.join();
    EXPECT_EQ(calls.load(), 1);
  }
}

TEST(CancellationTest, ConcurrentResetAndExecutionDeliverAtMostOnce) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    CancellationSource source;
    std::atomic<bool> start{false};
    std::atomic<int> calls{0};
    Registration registration = source.token().Register([&] { ++calls; });

    std::thread canceller([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      source.Cancel();
    });
    std::thread resetter([&] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      registration.Reset();
    });

    start.store(true, std::memory_order_release);
    resetter.join();
    canceller.join();
    EXPECT_LE(calls.load(), 1);
  }
}

}  // namespace
}  // namespace eos::lib

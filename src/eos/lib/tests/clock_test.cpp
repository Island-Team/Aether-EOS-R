#include "src/eos/lib/clock.h"

#include <chrono>
#include <thread>
#include <type_traits>
#include <vector>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

static_assert(!std::is_copy_constructible_v<ManualClock>);
static_assert(!std::is_copy_assignable_v<ManualClock>);
static_assert(!std::is_move_constructible_v<ManualClock>);
static_assert(!std::is_move_assignable_v<ManualClock>);

TEST(ClockTest, SystemClockMonotonicIsNonDecreasing) {
  SystemClock clock;
  const MonotonicTime a = clock.MonotonicNow();
  const MonotonicTime b = clock.MonotonicNow();
  EXPECT_LE(a, b);
}

TEST(ClockTest, ManualClockAdvancesBothClocks) {
  ManualClock clock;
  const WallTime wall0 = clock.WallNow();
  clock.Advance(std::chrono::milliseconds(250));
  EXPECT_EQ(clock.MonotonicNow() - MonotonicTime{}, std::chrono::milliseconds(250));
  EXPECT_EQ(clock.WallNow() - wall0, std::chrono::milliseconds(250));
}

TEST(ClockTest, ManualClockIsDeterministicWithoutAdvance) {
  ManualClock clock;
  EXPECT_EQ(clock.MonotonicNow(), clock.MonotonicNow());
  EXPECT_EQ(clock.WallNow(), clock.WallNow());
}

TEST(ClockTest, ManualClockRejectsNegativeAdvanceWithoutChangingTime) {
  ManualClock clock;
  clock.Advance(std::chrono::milliseconds(10));
  const MonotonicTime mono = clock.MonotonicNow();
  const WallTime wall = clock.WallNow();

  EXPECT_THROW(clock.Advance(std::chrono::nanoseconds(-1)), std::invalid_argument);
  EXPECT_EQ(clock.MonotonicNow(), mono);
  EXPECT_EQ(clock.WallNow(), wall);
}

TEST(ClockTest, ManualClockSupportsConcurrentReadsAndAdvances) {
  ManualClock clock;
  constexpr int kThreads = 4;
  constexpr int kAdvances = 5000;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&] {
      for (int i = 0; i < kAdvances; ++i) {
        clock.Advance(std::chrono::nanoseconds(1));
        (void)clock.MonotonicNow();
        (void)clock.WallNow();
      }
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(clock.MonotonicNow() - MonotonicTime{},
            std::chrono::nanoseconds(kThreads * kAdvances));
  EXPECT_EQ(clock.WallNow() - WallTime{}, std::chrono::nanoseconds(kThreads * kAdvances));
}

}  // namespace
}  // namespace eos::lib

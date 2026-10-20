#include "src/eos/lib/slot.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "gtest/gtest.h"

namespace eos::lib {
namespace {

using namespace std::chrono_literals;

struct Payload {
  std::uint64_t seq = 0;
  double value = 0.0;
};

TEST(SlotTest, ReadBeforePublishIsEmpty) {
  Slot<Payload> slot;
  EXPECT_FALSE(slot.Read(MonotonicTime{}, 1s).has_value());
  EXPECT_FALSE(slot.LastTimestamp().has_value());
}

TEST(SlotTest, FreshReadReturnsLatestValue) {
  Slot<Payload> slot;
  const MonotonicTime t0{};
  slot.Publish({1, 1.5}, t0);
  slot.Publish({2, 2.5}, t0 + 10ms);
  const auto out = slot.Read(t0 + 20ms, 50ms);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->seq, 2u);
  EXPECT_DOUBLE_EQ(out->value, 2.5);
}

TEST(SlotTest, StaleReadReturnsNullopt) {
  Slot<Payload> slot;
  const MonotonicTime t0{};
  slot.Publish({1, 1.0}, t0);
  // Consumer one budget past the publish: freshness expiry is a mode change,
  // not an error.
  EXPECT_FALSE(slot.Read(t0 + 2s, 1s).has_value());
}

TEST(SlotTest, ConcurrentPublishAndReadNeverBlocksAndStaysConsistent) {
  Slot<Payload> slot;
  ManualClock clock;
  std::atomic<bool> published{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> last_read{0};

  std::thread producer([&] {
    for (std::uint64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) {
      slot.Publish({i, 0.0}, clock.MonotonicNow());
      published.store(true, std::memory_order_release);
      clock.Advance(1ms);
    }
  });
  while (!published.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::thread consumer([&] {
    for (int i = 0; i < 10000; ++i) {
      const auto out = slot.Read(clock.MonotonicNow(), 1h);
      if (out.has_value()) {
        // Sequence numbers must never go backwards across reads.
        EXPECT_GE(out->seq, last_read.load(std::memory_order_relaxed));
        last_read.store(out->seq, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_relaxed);
  });

  consumer.join();
  producer.join();
  EXPECT_GT(last_read.load(), 0u);
}

}  // namespace
}  // namespace eos::lib

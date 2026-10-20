#include "src/eos/runtime/northbound/subscribe.h"

#include <gtest/gtest.h>

namespace eos::runtime::northbound {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

TEST(ChangeTrackerTest, IgnoresSequenceOnlyChanges) {
  ChangeTracker tracker(json{{"sequence", 1}, {"state", "idle"}});
  EXPECT_FALSE(tracker.Observe(json{{"sequence", 2}, {"state", "idle"}}, Clock::now()));
}

TEST(ChangeTrackerTest, IgnoresEnvelopeBookkeepingChanges) {
  ChangeTracker tracker(
      json{{"sequence", 1}, {"timestamp", "2026-09-23T00:00:00Z"}, {"state", "idle"}});
  EXPECT_FALSE(tracker.Observe(
      json{{"sequence", 2}, {"timestamp", "2026-09-23T00:00:01Z"}, {"state", "idle"}},
      Clock::time_point{} + std::chrono::seconds(1)));
}

TEST(ChangeTrackerTest, ReportsContentChangeAndSequence) {
  ChangeTracker tracker(json{{"sequence", 1}, {"state", "idle"}});
  const auto sequence = tracker.Observe(json{{"sequence", 2}, {"state", "moving"}},
                                        Clock::time_point{} + std::chrono::seconds(1));
  ASSERT_TRUE(sequence);
  EXPECT_EQ(*sequence, 2u);
}

TEST(ChangeTrackerTest, DefersChangesInsideThrottleWindow) {
  ChangeTracker tracker(json{{"sequence", 1}, {"state", "idle"}});
  const auto first = Clock::time_point{} + std::chrono::seconds(1);
  ASSERT_TRUE(tracker.Observe(json{{"sequence", 2}, {"state", "moving"}}, first));
  EXPECT_FALSE(tracker.Observe(json{{"sequence", 3}, {"state", "stopped"}},
                               first + std::chrono::milliseconds(50)));
  EXPECT_TRUE(tracker.Observe(json{{"sequence", 3}, {"state", "stopped"}}, first + kMinIntervalMs));
}

}  // namespace
}  // namespace eos::runtime::northbound

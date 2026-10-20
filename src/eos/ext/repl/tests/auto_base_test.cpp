#include "src/eos/ext/repl/auto_base.h"

#include <gtest/gtest.h>

namespace eos::ext::repl {
namespace {

TEST(AutoBaseKeysTest, EachPressProducesOnlyItsOwnTarget) {
  AutoBaseKeys keys;
  EXPECT_FALSE(keys.Press('z').active());
  EXPECT_DOUBLE_EQ(keys.Press('W').x_m, 0.24);
  EXPECT_DOUBLE_EQ(keys.Press('s').x_m, -0.24);
  EXPECT_DOUBLE_EQ(keys.Press('a').y_m, 0.24);
  EXPECT_DOUBLE_EQ(keys.Press('D').y_m, -0.24);
  EXPECT_DOUBLE_EQ(keys.Press('j').yaw_rad, 0.45);
  EXPECT_DOUBLE_EQ(keys.Press('K').yaw_rad, -0.45);
  const auto nudge = keys.Press('w');
  EXPECT_DOUBLE_EQ(nudge.y_m, 0);
  EXPECT_DOUBLE_EQ(nudge.yaw_rad, 0);
}

TEST(AutoBaseKeysTest, BracketsAdjustBoundedNudgeSpeed) {
  AutoBaseKeys keys;
  EXPECT_EQ(keys.SpeedPercent(), 100);
  EXPECT_TRUE(keys.AdjustSpeed('['));
  EXPECT_EQ(keys.SpeedPercent(), 75);
  EXPECT_DOUBLE_EQ(keys.Press('w').x_m, 0.18);
  for (int i = 0; i < 8; ++i) keys.AdjustSpeed('[');
  EXPECT_EQ(keys.SpeedPercent(), 25);
  for (int i = 0; i < 8; ++i) keys.AdjustSpeed(']');
  EXPECT_EQ(keys.SpeedPercent(), 200);
  EXPECT_DOUBLE_EQ(keys.Press('w').x_m, 0.48);
  EXPECT_DOUBLE_EQ(keys.Press('j').yaw_rad, 0.9);
  EXPECT_FALSE(keys.AdjustSpeed('z'));
}

}  // namespace
}  // namespace eos::ext::repl

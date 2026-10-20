#include "src/eos/runtime/northbound/duration.h"

#include <gtest/gtest.h>

namespace eos::runtime::northbound {
namespace {

TEST(DurationTest, AcceptsValidDurationComponents) {
  google::protobuf::Duration duration;
  EXPECT_TRUE(IsValidDuration(duration));

  duration.set_seconds(1);
  duration.set_nanos(999999999);
  EXPECT_TRUE(IsValidDuration(duration));

  duration.set_seconds(-1);
  duration.set_nanos(-999999999);
  EXPECT_TRUE(IsValidDuration(duration));
}

TEST(DurationTest, RejectsOutOfRangeOrMismatchedComponents) {
  google::protobuf::Duration duration;
  duration.set_seconds(google::protobuf::util::TimeUtil::kDurationMaxSeconds + 1);
  EXPECT_FALSE(IsValidDuration(duration));

  duration.set_seconds(0);
  duration.set_nanos(1000000000);
  EXPECT_FALSE(IsValidDuration(duration));

  duration.set_seconds(1);
  duration.set_nanos(-1);
  EXPECT_FALSE(IsValidDuration(duration));
}

}  // namespace
}  // namespace eos::runtime::northbound

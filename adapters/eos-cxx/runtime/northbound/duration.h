#pragma once

#include <google/protobuf/duration.pb.h>
#include <google/protobuf/util/time_util.h>

namespace eos::runtime::northbound {

inline bool IsValidDuration(const google::protobuf::Duration& duration) {
  const auto seconds = duration.seconds();
  const auto nanos = duration.nanos();
  return seconds >= google::protobuf::util::TimeUtil::kDurationMinSeconds &&
         seconds <= google::protobuf::util::TimeUtil::kDurationMaxSeconds &&
         nanos >= -999999999 && nanos <= 999999999 &&
         (seconds == 0 || nanos == 0 || (seconds < 0) == (nanos < 0));
}

}  // namespace eos::runtime::northbound

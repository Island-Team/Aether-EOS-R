#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace eos::runtime::skills::native::detail {

enum class PrimitiveTerminalStatus {
  kSucceeded,
  kFailed,
  kRejected,
  kCancelled,
  kTimedOut,
  kOutcomeUnknown,
  kInvalid
};

struct PrimitiveAcknowledgement {
  PrimitiveTerminalStatus status{PrimitiveTerminalStatus::kInvalid};
  std::string message;
};

PrimitiveAcknowledgement ParsePrimitiveAcknowledgement(const nlohmann::json& acknowledgement);

}  // namespace eos::runtime::skills::native::detail

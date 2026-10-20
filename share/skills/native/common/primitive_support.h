#pragma once

#include <functional>
#include <string>

#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::primitive {

using eos::runtime::skills::PrimitiveReference;
using eos::runtime::skills::builtin::Execution;
using eos::runtime::skills::builtin::Task;

enum class Status { kSucceeded, kRejected, kFailed, kCancelled, kTimedOut, kOutcomeUnknown };

struct Result {
  Status status{Status::kFailed};
  std::string message;
};

Result ParseAcknowledgement(const nlohmann::json& acknowledgement);

using Poll = std::function<eos::runtime::skills::builtin::Result<bool>()>;

Task<Result> Run(Execution& execution, PrimitiveReference primitive,
                 const std::string& device_id, nlohmann::json arguments,
                 Poll poll = {});

}  // namespace island::skills::primitive

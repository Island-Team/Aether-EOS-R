#include "src/eos/runtime/skills/core/model.h"

namespace eos::runtime::skills {
const char* ExecutionStatusName(ExecutionStatus status) {
  switch (status) {
    case ExecutionStatus::kQueued:
      return "queued";
    case ExecutionStatus::kRunning:
      return "running";
    case ExecutionStatus::kSucceeded:
      return "succeeded";
    case ExecutionStatus::kRejected:
      return "rejected";
    case ExecutionStatus::kCancelled:
      return "cancelled";
    case ExecutionStatus::kTimedOut:
      return "timed_out";
    case ExecutionStatus::kFailed:
      return "failed";
  }
  return "failed";
}
bool IsTerminal(ExecutionStatus status) {
  return status == ExecutionStatus::kSucceeded || status == ExecutionStatus::kRejected ||
         status == ExecutionStatus::kCancelled || status == ExecutionStatus::kTimedOut ||
         status == ExecutionStatus::kFailed;
}
}  // namespace eos::runtime::skills

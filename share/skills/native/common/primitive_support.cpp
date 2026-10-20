#include "share/skills/native/common/primitive_support.h"

#include <chrono>
#include <utility>

namespace island::skills::primitive {
using namespace std::chrono_literals;

constexpr auto kPollInterval = 50ms;

Result ParseAcknowledgement(const nlohmann::json& acknowledgement) {
  try {
    const bool accepted = acknowledgement.at("accepted").get<bool>();
    const auto result = acknowledgement.value("result", nlohmann::json::object());
    const std::string status = result.value("status", "");
    const std::string reason = result.value("reason", "");
    if (status == "cancelled") return {Status::kCancelled, reason};
    if (!accepted && status == "rejected")
      return {Status::kRejected,
              acknowledgement.value("reason", "primitive request was rejected")};
    if (accepted && status == "succeeded") return {Status::kSucceeded, {}};
    if (!accepted)
      return {Status::kFailed, "primitive acknowledgement was inconsistent"};
    if (status == "timed_out") return {Status::kTimedOut, reason};
    if (status == "outcome_unknown") return {Status::kOutcomeUnknown, reason};
    return {Status::kFailed, reason.empty() ? "primitive execution failed" : reason};
  } catch (...) {
    return {Status::kFailed, "primitive acknowledgement was malformed"};
  }
}

Task<Result> Run(Execution& execution, PrimitiveReference primitive,
                 const std::string& device_id, nlohmann::json arguments, Poll poll) {
  auto started = execution.StartPrimitive(primitive, device_id, std::move(arguments));
  if (!started) co_return Result{Status::kFailed, started.error().message};

  bool cancel_requested = false;
  std::string cancel_error;
  std::string poll_error;
  while (true) {
    if (execution.cancelled() && !cancel_requested) {
      cancel_requested = true;
      auto cancelled = execution.CancelPrimitive(started.value());
      if (!cancelled) cancel_error = cancelled.error().message;
    }
    if (!cancel_requested && poll) {
      auto ready = poll();
      if (!ready) {
        poll_error = ready.error().message;
        auto cancelled = execution.CancelPrimitive(started.value());
        if (!cancelled) cancel_error = cancelled.error().message;
        cancel_requested = true;
      } else if (!ready.value()) {
        co_await execution.DelayFor(kPollInterval);
        continue;
      }
    }
    auto waited = execution.WaitPrimitive(started.value(), kPollInterval);
    if (!waited) {
      if (!poll_error.empty()) co_return Result{Status::kFailed, poll_error};
      co_return Result{cancel_requested ? Status::kCancelled : Status::kFailed,
                       cancel_error.empty() ? waited.error().message : cancel_error};
    }
    if (!waited.value()) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }
    if (!poll_error.empty()) co_return Result{Status::kFailed, poll_error};
    if (cancel_requested)
      co_return Result{Status::kCancelled,
                       cancel_error.empty() ? "skill execution was cancelled" : cancel_error};
    co_return ParseAcknowledgement(*waited.value());
  }
}

}  // namespace island::skills::primitive

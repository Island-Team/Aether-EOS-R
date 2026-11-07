#include "common.h"
#include "dispatcher.h"

namespace eos::runtime::dispatch {
nlohmann::json Dispatcher::Ack(const std::string& primitive, const std::string& version,
                               const nlohmann::json& arguments, bool accepted,
                               const char* reason_code, const std::string& reason) {
  const auto command_id = arguments.find("command_id");
  return {{"type", "primitive.ack"},
          {"primitive", primitive},
          {"primitive_version", version},
          {"accepted", accepted},
          {"reason", accepted ? nlohmann::json() : nlohmann::json(reason)},
          {"reason_code", accepted ? nlohmann::json() : nlohmann::json(reason_code)},
          {"command_id", command_id == arguments.end() ? nlohmann::json() : *command_id},
          {"sequence", ++ack_sequence_},
          {"result", {{"status", accepted ? "succeeded" : "rejected"}}}};
}

nlohmann::json Dispatcher::Accepted(const std::string& primitive, const std::string& version,
                                    const nlohmann::json& arguments) {
  return Ack(primitive, version, arguments, true, nullptr, "");
}

nlohmann::json Dispatcher::Released(const std::string& primitive, const std::string& version,
                                    const nlohmann::json& arguments,
                                    hardware_runtime::Device& device,
                                    hardware::ReleaseResult result,
                                    const std::string& fallback_reason) {
  if (result == hardware::ReleaseResult::kAccepted) {
    return Accepted(primitive, version, arguments);
  }
  std::string reason = device.backend().ReleaseFailureReason();
  if (reason.empty() || reason == "backend refused the release") reason = fallback_reason;
  if (result == hardware::ReleaseResult::kTimedOut) {
    if (reason.empty()) reason = "backend release timed out";
    nlohmann::json ack = Accepted(primitive, version, arguments);
    ack["result"] = {{"status", "timed_out"}, {"reason", reason}};
    return ack;
  }
  if (result == hardware::ReleaseResult::kFailed || result == hardware::ReleaseResult::kCancelled ||
      result == hardware::ReleaseResult::kOutcomeUnknown) {
    nlohmann::json ack = Accepted(primitive, version, arguments);
    const char* status = result == hardware::ReleaseResult::kFailed      ? "failed"
                         : result == hardware::ReleaseResult::kCancelled ? "cancelled"
                                                                         : "outcome_unknown";
    ack["result"] = {{"status", status}, {"reason", reason}};
    return ack;
  }
  if (reason.empty()) reason = "backend refused the release";
  return Ack(primitive, version, arguments, false, detail::kInvalidState, reason);
}
}  // namespace eos::runtime::dispatch

#include <chrono>

#include "common.h"
#include "dispatcher.h"

namespace eos::runtime::dispatch {
using namespace detail;
nlohmann::json Dispatcher::GimbalMotion(const std::string& primitive, const std::string& version,
                                        hardware_runtime::Device& device,
                                        const nlohmann::json& arguments) {
  const hardware_runtime::PrimitiveDeclaration* declaration = DeclarationOf(device, primitive);
  const auto requested_reference = arguments.find("reference");
  if (declaration == nullptr || requested_reference == arguments.end() ||
      !requested_reference->is_string() ||
      !SupportsReference(*declaration, requested_reference->get<std::string>())) {
    return Ack(primitive, version, arguments, false, kUnsupportedReference,
               "gimbal reference is not admitted for this device");
  }
  hardware::GimbalCommand command;
  command.command_id = arguments.value("command_id", "");
  command.stream = arguments.value("stream", false);
  const auto pan = arguments.find("pan_rad");
  const auto tilt = arguments.find("tilt_rad");
  if (pan == arguments.end() || tilt == arguments.end() || !pan->is_number() ||
      !tilt->is_number()) {
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "pan_rad and tilt_rad must be finite numbers");
  }
  command.pan_rad = pan->get<double>();
  command.tilt_rad = tilt->get<double>();
  if (*requested_reference == "relative") {
    std::optional<GimbalStreamTarget> streamed_target;
    if (command.stream) {
      std::lock_guard<std::mutex> lock(state_mutex_);
      const auto found = gimbal_stream_targets_.find(device.identity().device_id);
      if (found != gimbal_stream_targets_.end()) streamed_target = found->second;
    }
    const auto current = streamed_target ? std::optional<hardware::GimbalTelemetry>{}
                                         : device.feeds().ReadGimbalTelemetry();
    if (!streamed_target && !current) {
      return Ack(primitive, version, arguments, false, kInvalidState,
                 "relative gimbal motion requires fresh gimbal state");
    }
    command.pan_rad += streamed_target ? streamed_target->pan_rad : current->pan_rad;
    command.tilt_rad += streamed_target ? streamed_target->tilt_rad : current->tilt_rad;
  }
  const hardware_runtime::NumericRange* pan_range = LimitOf(device, primitive, "pan_rad");
  const hardware_runtime::NumericRange* tilt_range = LimitOf(device, primitive, "tilt_rad");
  if (pan_range == nullptr || tilt_range == nullptr || !pan_range->Contains(command.pan_rad) ||
      !tilt_range->Contains(command.tilt_rad)) {
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "resolved pan_rad or tilt_rad is outside the admitted device range");
  }
  if (!OptionalInAdmittedRange(arguments, "speed_rad_s", device, primitive, &command.speed_rad_s))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "speed_rad_s is outside the admitted device range");
  command.stamp = std::chrono::steady_clock::now();
  const hardware::ReleaseResult released = device.backend().ExecuteGimbal(command);
  if (released == hardware::ReleaseResult::kAccepted) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (command.stream) {
      gimbal_stream_targets_.insert_or_assign(
          device.identity().device_id,
          GimbalStreamTarget{.pan_rad = command.pan_rad, .tilt_rad = command.tilt_rad});
    } else {
      gimbal_stream_targets_.erase(device.identity().device_id);
    }
  }
  return Released(primitive, version, arguments, device, released, "gimbal refused the release");
}
}  // namespace eos::runtime::dispatch

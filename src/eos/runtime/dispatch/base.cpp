#include <chrono>

#include "common.h"
#include "dispatcher.h"
#include "src/eos/lib/log.h"

namespace eos::runtime::dispatch {
using namespace detail;

std::optional<std::array<double, 3>> detail::ResolveBaseStreamMapTarget(
    const hardware::BaseTelemetry& pose, bool absolute, double x_m, double y_m, double yaw_rad) {
  if (!pose.valid || !pose.fresh || !pose.pose_valid || !pose.localization_valid ||
      !std::isfinite(pose.x_m) || !std::isfinite(pose.y_m) || !std::isfinite(pose.yaw_rad)) {
    return std::nullopt;
  }
  if (absolute) return std::array<double, 3>{x_m, y_m, yaw_rad};
  const double cosine = std::cos(pose.yaw_rad);
  const double sine = std::sin(pose.yaw_rad);
  return std::array<double, 3>{pose.x_m + cosine * x_m - sine * y_m,
                               pose.y_m + sine * x_m + cosine * y_m,
                               std::remainder(pose.yaw_rad + yaw_rad, 6.28318530717958647692)};
}

nlohmann::json Dispatcher::BaseVelocity(const std::string& primitive, const std::string& version,
                                        hardware_runtime::Device& device,
                                        const nlohmann::json& arguments) {
  double x = 0.0, y = 0.0, yaw = 0.0;
  const auto lease = arguments.find("valid_for_ms");
  double lease_ms = 0.0;
  if (!InAdmittedRange(arguments, "linear_x_m_s", device, primitive, &x) ||
      !InAdmittedRange(arguments, "linear_y_m_s", device, primitive, &y) ||
      !InAdmittedRange(arguments, "angular_z_rad_s", device, primitive, &yaw) ||
      lease == arguments.end() || !lease->is_number_integer() ||
      !InAdmittedRange(arguments, "valid_for_ms", device, primitive, &lease_ms))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "invalid base velocity arguments");
  hardware::BaseCommand command{x, y, yaw, std::chrono::steady_clock::now(),
                                arguments.value("command_id", "")};
  const hardware::ReleaseResult released = device.backend().ExecuteBase(command);
  if (released != hardware::ReleaseResult::kAccepted) {
    return Released(primitive, version, arguments, device, released,
                    "base refused the velocity release");
  }
  const std::string admitted_device_id = device.backend().Identify().device_id;
  bool replaced = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    replaced = base_velocity_lease_s_.find(admitted_device_id) != base_velocity_lease_s_.end();
    base_velocity_lease_s_[admitted_device_id] = lease->get<double>() / 1000.0;
  }
  lib::LogEvent(lib::GetLogger("dispatch"), spdlog::level::debug, "velocity_lease_started",
                {{"device_id", admitted_device_id},
                 {"valid_for_ms", lease->get<std::int64_t>()},
                 {"replaced", replaced}});
  return Accepted(primitive, version, arguments);
}

nlohmann::json Dispatcher::BaseNavigate(const std::string& primitive, const std::string& version,
                                        hardware_runtime::Device& device,
                                        const nlohmann::json& arguments) {
  const hardware_runtime::PrimitiveDeclaration* declaration = DeclarationOf(device, primitive);
  const auto requested_reference = arguments.find("reference");
  if (declaration == nullptr || requested_reference == arguments.end() ||
      !requested_reference->is_string() ||
      !SupportsReference(*declaration, requested_reference->get<std::string>())) {
    return Ack(primitive, version, arguments, false, kUnsupportedReference,
               "navigation reference is not admitted for this device");
  }
  double x = 0.0, y = 0.0, yaw = 0.0;
  if (!InAdmittedRange(arguments, "x_m", device, primitive, &x) ||
      !InAdmittedRange(arguments, "y_m", device, primitive, &y) ||
      !InAdmittedRange(arguments, "yaw_rad", device, primitive, &yaw))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "invalid navigation arguments");
  std::optional<double> z_m;
  if (!OptionalInAdmittedRange(arguments, "z_m", device, primitive, &z_m))
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "z_m is outside the admitted device range");
  std::optional<double> speed;
  if (const auto value = arguments.find("speed"); value != arguments.end()) {
    double admitted_speed = 0.0;
    if (!value->is_number_integer() || !InAdmittedRange(arguments, "speed", device, primitive,
                                                        &admitted_speed) ||
        admitted_speed < 0.0 || admitted_speed > 2.0)
      return Ack(primitive, version, arguments, false, kInvalidArgument,
                 "speed must be admitted as 0, 1, or 2");
    speed = admitted_speed;
  }
  const bool streaming = arguments.value("stream", false);
  std::optional<std::array<double, 3>> stream_target;
  if (*requested_reference == "absolute" || streaming) {
    const auto frame = arguments.find("frame_id");
    if (*requested_reference == "absolute" &&
        (!declaration->absolute_frame.has_value() || frame == arguments.end() ||
         !frame->is_string() || *frame != *declaration->absolute_frame)) {
      return Ack(primitive, version, arguments, false, kUnsupportedReference,
                 "absolute navigation frame is not admitted for this device");
    }
    const auto current = device.feeds().ReadBaseTelemetry();
    if (!current.has_value() || !current->valid || !current->fresh || !current->pose_valid ||
        (streaming && !current->localization_valid)) {
      return Ack(primitive, version, arguments, false, kInvalidState,
                 "absolute navigation requires fresh base state");
    }
    if (streaming) {
      stream_target =
          ResolveBaseStreamMapTarget(*current, *requested_reference == "absolute", x, y, yaw);
      if (!stream_target.has_value())
        return Ack(primitive, version, arguments, false, kInvalidState,
                   "stream navigation requires fresh localization");
    }
    if (*requested_reference == "absolute") {
      if (!stream_target.has_value()) stream_target = std::array<double, 3>{x, y, yaw};
      const double dx = x - current->x_m;
      const double dy = y - current->y_m;
      const double cosine = std::cos(current->yaw_rad);
      const double sine = std::sin(current->yaw_rad);
      x = cosine * dx + sine * dy;
      y = -sine * dx + cosine * dy;
      constexpr double kTwoPi = 6.28318530717958647692;
      yaw = std::remainder(yaw - current->yaw_rad, kTwoPi);
    }
  }
  const hardware::BaseNavigateRelativeCommand command{x,
                                                      y,
                                                      yaw,
                                                      std::chrono::steady_clock::now(),
                                                      arguments.value("command_id", ""),
                                                      streaming,
                                                      stream_target,
                                                      *requested_reference == "absolute",
                                                      z_m,
                                                      speed};
  return Released(primitive, version, arguments, device,
                  device.backend().ExecuteNavigateRelative(command),
                  "base refused the navigation release");
}

nlohmann::json Dispatcher::BasePosture(const std::string& primitive, const std::string& version,
                                       hardware_runtime::Device& device,
                                       const nlohmann::json& arguments) {
  const auto value = arguments.find("posture");
  if (value == arguments.end() || !value->is_string())
    return Ack(primitive, version, arguments, false, kInvalidArgument, "posture must be a string");
  hardware::BasePosture posture;
  if (*value == "stand")
    posture = hardware::BasePosture::kStand;
  else if (*value == "walk")
    posture = hardware::BasePosture::kWalk;
  else if (*value == "lie")
    posture = hardware::BasePosture::kLie;
  else
    return Ack(primitive, version, arguments, false, kInvalidArgument, "invalid posture");
  const hardware::BasePostureCommand command{posture, arguments.value("command_id", "")};
  return Released(primitive, version, arguments, device, device.backend().ExecutePosture(command),
                  "base refused the posture release");
}
}  // namespace eos::runtime::dispatch

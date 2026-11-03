#include "socket_backend.h"
#include "socket_protocol.h"

#include <cstdlib>

namespace eos::backends::ros2 {
using Json = nlohmann::json;
using namespace eos::hardware;
namespace {
template <typename T> Json Opt(const std::optional<T>& value) { return value ? Json(*value) : Json(nullptr); }
template <typename T> std::optional<T> GetOpt(const Json& value, const char* key) {
  if (!value.contains(key) || value[key].is_null()) return std::nullopt;
  return value[key].get<T>();
}
ReleaseResult Result(const Json& reply) {
  const int value = reply.value("result", static_cast<int>(ReleaseResult::kFailed));
  if (value < 0 || value > static_cast<int>(ReleaseResult::kOutcomeUnknown))
    return ReleaseResult::kFailed;
  return static_cast<ReleaseResult>(value);
}
}
SocketBackend::SocketBackend(std::string id, DeviceKind kind, std::string path)
    : device_id_(std::move(id)), kind_(kind), socket_path_(std::move(path)) {}
BackendIdentity SocketBackend::Identify() const {
  return {"ros2-bridge", "1.0.0", device_id_, kind_};
}
FeedDeclarations SocketBackend::DeclareFeeds() const {
  FeedDeclarations feeds;
  if (kind_ == DeviceKind::kCamera) feeds.sensor.max_rate_hz = 30.0;
  else feeds.proprioception.max_rate_hz = 10.0;
  feeds.health.max_rate_hz = 10.0;
  return feeds;
}
Json SocketBackend::Request(const std::string& operation, Json arguments) const {
  try {
    return socket_protocol::Exchange(socket_path_, {{"v", socket_protocol::kVersion},
                                  {"device", device_id_}, {"op", operation},
                                  {"args", std::move(arguments)}},
                                  operation == "health" || operation.find("telemetry") != std::string::npos ||
                                  operation == "manipulator" ? 2 : 35);
  } catch (const std::exception& error) {
    std::lock_guard<std::mutex> guard(error_mutex_);
    last_error_ = std::string("ROS bridge unavailable: ") + error.what();
    throw;
  }
}
BackendHealth SocketBackend::ReadHealth() const {
  try {
    const Json value = Request("health");
    return {value.value("alive", false), IsAdmitted()};
  } catch (...) { return {false, IsAdmitted()}; }
}
ReleaseResult SocketBackend::Release(const std::string& operation, Json arguments) {
  try { return Result(Request(operation, std::move(arguments))); }
  catch (...) { return ReleaseResult::kOutcomeUnknown; }
}
ReleaseResult SocketBackend::ExecuteMoveJ(const MoveJCommand& cmd) {
  return Release("movej", {{"positions", cmd.joint_positions_rad}, {"speed", Opt(cmd.speed_rad_s)},
                           {"command_id", cmd.command_id}, {"stream", cmd.stream}});
}
ReleaseResult SocketBackend::ExecuteMoveL(const MoveLCommand& cmd) {
  return Release("movel", {{"position", {cmd.position_m[0], cmd.position_m[1], cmd.position_m[2]}},
                           {"orientation", {cmd.orientation_quat_xyzw[0], cmd.orientation_quat_xyzw[1],
                                             cmd.orientation_quat_xyzw[2], cmd.orientation_quat_xyzw[3]}},
                           {"speed", Opt(cmd.speed_m_s)}, {"command_id", cmd.command_id},
                           {"stream", cmd.stream}});
}
ReleaseResult SocketBackend::ExecutePick(const PickCommand& cmd) {
  return Release("pick", {{"object_id", cmd.object_id}, {"command_id", cmd.command_id}});
}
ReleaseResult SocketBackend::ExecuteGimbal(const GimbalCommand& cmd) {
  return Release("gimbal", {{"pan", cmd.pan_rad}, {"tilt", cmd.tilt_rad},
                            {"speed", Opt(cmd.speed_rad_s)}, {"command_id", cmd.command_id},
                            {"stream", cmd.stream}});
}
ReleaseResult SocketBackend::ExecuteGripper(const GripperCommand& cmd) {
  return Release("gripper", {{"action", static_cast<int>(cmd.action)}, {"force", Opt(cmd.force_n)},
                            {"opening", Opt(cmd.opening_pct)}, {"speed", Opt(cmd.speed_pct)},
                            {"command_id", cmd.command_id}});
}
ReleaseResult SocketBackend::StopMotion() { return Release("stop"); }
ReleaseResult SocketBackend::CancelCommand(const std::string& id) {
  return Release("cancel", {{"command_id", id}});
}
std::optional<ManipulatorTelemetry> SocketBackend::ReadManipulatorTelemetry() const {
  try {
    auto r = Request("manipulator");
    if (r["data"].is_null()) return std::nullopt;
    const auto& j = r["data"];
    ManipulatorTelemetry t;
    t.joint_angles_rad = j.at("angles").get<std::array<double, 6>>();
    t.joint_velocities_rad_s = j.at("velocities").get<std::array<double, 6>>();
    t.joint_valid = j.at("joint_valid").get<std::array<bool, 6>>();
    t.joint_velocity_valid = j.at("velocity_valid").get<std::array<bool, 6>>();
    for (int i = 0; i < 3; ++i) t.position_m[i] = j.at("position").at(i).get<double>();
    for (int i = 0; i < 4; ++i) t.orientation_quat[i] = j.at("orientation").at(i).get<double>();
    t.pose_valid = j.value("pose_valid", false);
    t.status_valid = j.value("status_valid", false);
    t.status_source_timestamp_ns = GetOpt<std::int64_t>(j,"status_timestamp");
    t.controller_state = j.value("controller_state", "unknown");
    t.controller_status_code = GetOpt<std::int64_t>(j,"controller_status_code");
    t.controller_error_codes = j.value("controller_error_codes", std::vector<std::int64_t>{});
    if (j.contains("joint_diagnostics")) for (std::size_t i = 0; i < 6; ++i) {
      const auto& d = j["joint_diagnostics"][i];
      t.joint_diagnostics[i].enabled = GetOpt<bool>(d,"enabled");
      t.joint_diagnostics[i].error_code = GetOpt<std::int64_t>(d,"error_code");
      t.joint_diagnostics[i].motor_current_raw = GetOpt<double>(d,"motor_current");
      t.joint_diagnostics[i].temperature_raw = GetOpt<double>(d,"temperature");
      t.joint_diagnostics[i].voltage_raw = GetOpt<double>(d,"voltage");
    }
    return t;
  } catch (...) { return std::nullopt; }
}
std::optional<GimbalTelemetry> SocketBackend::ReadGimbalTelemetry() const {
  try { auto j = Request("gimbal_telemetry")["data"]; if (j.is_null()) return std::nullopt;
    return GimbalTelemetry{j.at("pan").get<double>(), j.at("tilt").get<double>(),
                           j.at("pan_rate").get<double>(), j.at("tilt_rate").get<double>()};
  } catch (...) { return std::nullopt; }
}
std::optional<GripperTelemetry> SocketBackend::ReadGripperTelemetry() const {
  try { auto j = Request("gripper_telemetry")["data"]; if (j.is_null()) return std::nullopt;
    GripperTelemetry t; t.state = static_cast<GripperState>(j.at("state").get<int>());
    t.force_n = GetOpt<double>(j,"force");
    t.position_pct = GetOpt<double>(j,"position"); return t;
  } catch (...) { return std::nullopt; }
}
std::optional<CameraTelemetry> SocketBackend::ReadCameraTelemetry() const {
  try { auto j = Request("camera_telemetry")["data"]; if (j.is_null()) return std::nullopt;
    CameraTelemetry t; t.width_px = j.at("width").get<std::uint32_t>();
    t.height_px = j.at("height").get<std::uint32_t>(); t.frame_id = j.value("frame_id", "");
    t.source_timestamp_ns = GetOpt<std::int64_t>(j,"source_timestamp");
    t.source_clock = static_cast<SourceClockKind>(j.value("source_clock", 0));
    t.source_clock_id = j.value("source_clock_id", "");
    t.jpeg = j.at("jpeg").get<std::vector<std::uint8_t>>();
    t.depth_png = j.value("depth_png", std::vector<std::uint8_t>{});
    t.depth_u16 = j.value("depth_u16", std::vector<std::uint16_t>{});
    t.depth_scale = j.value("depth_scale", 0.001);
    if (j.contains("depth_intrinsics") && !j["depth_intrinsics"].is_null()) {
      const auto& c = j["depth_intrinsics"];
      CameraIntrinsics i; i.width_px = c.value("width", 0U); i.height_px = c.value("height", 0U);
      i.fx = c.value("fx", 0.0); i.fy = c.value("fy", 0.0); i.ppx = c.value("ppx", 0.0);
      i.ppy = c.value("ppy", 0.0); i.distortion_model = c.value("distortion_model", "");
      i.distortion_coefficients = c.value("distortion_coefficients", std::array<double, 5>{});
      t.depth_intrinsics = i;
    }
    if (j.contains("capture_gimbal") && !j["capture_gimbal"].is_null()) {
      auto g = j["capture_gimbal"];
      t.capture_gimbal = GimbalTelemetry{g.at("pan").get<double>(), g.at("tilt").get<double>(),
                                         g.at("pan_rate").get<double>(), g.at("tilt_rate").get<double>()};
    }
    t.capture_gimbal_sampled_monotonic_ns = GetOpt<std::int64_t>(j,"capture_gimbal_sampled_monotonic_ns");
    return t;
  } catch (...) { return std::nullopt; }
}
std::string SocketBackend::ReleaseFailureReason() const {
  try { return Request("reason").value("reason", "bridge refused release"); }
  catch (...) { std::lock_guard<std::mutex> guard(error_mutex_); return last_error_; }
}
void SocketBackend::Clamp() { try { Request("clamp"); } catch (...) {} }
void SocketBackend::Halt() { try { Request("halt"); } catch (...) {} }
}  // namespace eos::backends::ros2

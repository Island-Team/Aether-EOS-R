#include "detail.h"
#include "reader.h"
namespace eos::runtime::perception::body {
using namespace detail;
namespace {

const char* MotionState(std::int64_t value) {
  switch (value) {
    case 0:
      return "prone";
    case 1:
      return "standing_up";
    case 2:
      return "initial_standing";
    case 3:
      return "force_control_standing";
    case 4:
      return "stepping";
    case 5:
      return "lying_down";
    case 6:
      return "emergency_or_fallen";
    case 16:
      return "reinforcement_learning";
    default:
      return "unknown";
  }
}

const char* BinaryState(std::int64_t value, const char* false_state, const char* true_state) {
  if (value == 0) return false_state;
  if (value == 1) return true_state;
  return "unknown";
}

}  // namespace

nlohmann::json Reader::BuildBase(const std::string& part_id) {
  const hardware_runtime::Device* device =
      FindDevice(registry_, DeviceKind::kBase, "base", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("base", part_id);
  }
  return DeviceSummaryOf(*device, "base");
}

nlohmann::json Reader::BuildBaseStatus(const std::string& robot_id) {
  const hardware_runtime::Device* device = FindRobotDevice(registry_, robot_id, DeviceKind::kBase);
  if (device == nullptr) {
    ThrowAbsentPart("base", robot_id);
  }

  const DeviceStatus status = ProjectDeviceStatus(*device);
  nlohmann::json snapshot = StartEnvelope("base.status", "map", device->identity().device_id,
                                          &device->part(), "base/" + robot_id + "/status");
  snapshot["state"] = status.state;
  snapshot["health"] = status.health;
  const auto telemetry = device->feeds().ReadBaseTelemetry();
  snapshot["telemetry_valid"] = telemetry.has_value();
  if (telemetry) {
    snapshot["telemetry_valid"] = telemetry->valid;
    snapshot["fresh"] = telemetry->fresh;
    snapshot["pose_valid"] = telemetry->pose_valid;
    snapshot["localization_valid"] = telemetry->localization_valid;
    if (telemetry->observed_at != std::chrono::system_clock::time_point{})
      snapshot["observed_at"] = TimestampOf(telemetry->observed_at);
    if (telemetry->pose_valid) {
      nlohmann::json pose = {
          {"x_m", telemetry->x_m}, {"y_m", telemetry->y_m}, {"yaw_rad", telemetry->yaw_rad}};
      if (telemetry->z_m) pose["z_m"] = *telemetry->z_m;
      if (telemetry->roll_rad) pose["roll_rad"] = *telemetry->roll_rad;
      if (telemetry->pitch_rad) pose["pitch_rad"] = *telemetry->pitch_rad;
      snapshot["pose"] = std::move(pose);
    }
    if (telemetry->forward_speed_m_s) snapshot["forward_speed_m_s"] = *telemetry->forward_speed_m_s;
    if (telemetry->battery_percent) snapshot["battery_percent"] = *telemetry->battery_percent;
    if (telemetry->current_odometry_m)
      snapshot["current_odometry_m"] = *telemetry->current_odometry_m;
    if (telemetry->total_odometry_m) snapshot["total_odometry_m"] = *telemetry->total_odometry_m;
    if (telemetry->map_resolution_m_per_pixel)
      snapshot["map_resolution_m_per_pixel"] = *telemetry->map_resolution_m_per_pixel;
    if (telemetry->map_origin_x_m) snapshot["map_origin_x_m"] = *telemetry->map_origin_x_m;
    if (telemetry->map_origin_y_m) snapshot["map_origin_y_m"] = *telemetry->map_origin_y_m;
    if (telemetry->map_height_pixels) snapshot["map_height_pixels"] = *telemetry->map_height_pixels;
    const auto motion = telemetry->diagnostics.find("motion_state");
    if (motion != telemetry->diagnostics.end())
      snapshot["motion_state"] = MotionState(motion->second);
    const auto location = telemetry->diagnostics.find("location");
    snapshot["localization_state"] = location == telemetry->diagnostics.end()
                                         ? "unknown"
                                         : BinaryState(location->second, "normal", "lost");
    const auto charge = telemetry->diagnostics.find("charge_state");
    if (charge != telemetry->diagnostics.end())
      snapshot["charging_state"] = BinaryState(charge->second, "not_charging", "charging");
    const auto dock = telemetry->diagnostics.find("dock_state");
    if (dock != telemetry->diagnostics.end())
      snapshot["dock_state"] = BinaryState(dock->second, "undocked", "docked");
    snapshot["diagnostics"] = telemetry->diagnostics;
  }
  return snapshot;
}

}  // namespace eos::runtime::perception::body

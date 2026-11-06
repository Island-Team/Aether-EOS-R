#include <nlohmann/json.hpp>

#include "detail.h"
#include "reader.h"

namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildManipulator(const std::string& part_id) {
  const hardware_runtime::Device* device =
      FindDevice(registry_, DeviceKind::kManipulator, "arm", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("manipulator", part_id);
  }
  return DeviceSummaryOf(*device, "manipulator");
}

nlohmann::json Reader::BuildArmState(const std::string& robot_id) {
  const hardware_runtime::Device* device =
      FindRobotDevice(registry_, robot_id, DeviceKind::kManipulator);
  if (device == nullptr) {
    ThrowAbsentPart("arm", robot_id);
  }
  const auto telemetry = device->feeds().ReadManipulatorTelemetry();
  if (!telemetry) {
    throw ResourceUnavailable("arm telemetry is unavailable for robot '" + robot_id + "'");
  }

  nlohmann::json joints = nlohmann::json::array();
  nlohmann::json valid = nlohmann::json::array();
  for (std::size_t index = 0; index < telemetry->joint_angles_rad.size(); ++index) {
    valid.push_back(telemetry->joint_valid[index]);
    joints.push_back(telemetry->joint_valid[index]
                         ? nlohmann::json(telemetry->joint_angles_rad[index])
                         : nlohmann::json(nullptr));
  }

  nlohmann::json snapshot = StartEnvelope("arm.snapshot", "base_link", device->identity().device_id,
                                          &device->part(), "arm/" + robot_id + "/state");
  snapshot["joint_positions_rad"] = std::move(joints);
  snapshot["joint_valid"] = std::move(valid);
  snapshot["pose_valid"] = telemetry->pose_valid;
  return snapshot;
}

nlohmann::json Reader::BuildArmStatus(const std::string& robot_id) {
  const hardware_runtime::Device* device =
      FindRobotDevice(registry_, robot_id, DeviceKind::kManipulator);
  if (device == nullptr) {
    ThrowAbsentPart("arm", robot_id);
  }

  const DeviceStatus status = ProjectDeviceStatus(*device);
  nlohmann::json snapshot = StartEnvelope("arm.status", "base_link", device->identity().device_id,
                                          &device->part(), "arm/" + robot_id + "/status");
  snapshot["state"] = status.state;
  snapshot["health"] = status.health;
  const auto telemetry = device->feeds().ReadManipulatorTelemetry();
  snapshot["telemetry_valid"] = telemetry.has_value();
  if (!telemetry) return snapshot;

  nlohmann::json positions = nlohmann::json::array();
  nlohmann::json velocities = nlohmann::json::array();
  nlohmann::json valid = nlohmann::json::array();
  nlohmann::json velocity_valid = nlohmann::json::array();
  for (std::size_t index = 0; index < telemetry->joint_angles_rad.size(); ++index) {
    valid.push_back(telemetry->joint_valid[index]);
    velocity_valid.push_back(telemetry->joint_velocity_valid[index]);
    positions.push_back(telemetry->joint_valid[index]
                            ? nlohmann::json(telemetry->joint_angles_rad[index])
                            : nlohmann::json(nullptr));
    velocities.push_back(telemetry->joint_velocity_valid[index]
                             ? nlohmann::json(telemetry->joint_velocities_rad_s[index])
                             : nlohmann::json(nullptr));
  }
  if (!device->part().joint_names.empty()) snapshot["joint_names"] = device->part().joint_names;
  snapshot["joint_positions_rad"] = std::move(positions);
  snapshot["joint_velocities_rad_s"] = std::move(velocities);
  snapshot["joint_valid"] = std::move(valid);
  snapshot["joint_velocity_valid"] = std::move(velocity_valid);
  snapshot["pose_valid"] = telemetry->pose_valid;
  if (telemetry->pose_valid) {
    snapshot["end_effector_pose"] = {
        {"position_m",
         {telemetry->position_m[0], telemetry->position_m[1], telemetry->position_m[2]}},
        {"orientation_quat_xyzw",
         {telemetry->orientation_quat[0], telemetry->orientation_quat[1],
          telemetry->orientation_quat[2], telemetry->orientation_quat[3]}}};
  }
  snapshot["status_valid"] = telemetry->status_valid;
  if (telemetry->status_valid) {
    snapshot["controller_state"] = telemetry->controller_state;
    if (telemetry->status_source_timestamp_ns)
      snapshot["status_source_timestamp_ns"] = *telemetry->status_source_timestamp_ns;
    if (telemetry->controller_status_code)
      snapshot["controller_status_code"] = *telemetry->controller_status_code;
    snapshot["controller_error_codes"] = telemetry->controller_error_codes;
    nlohmann::json diagnostics = nlohmann::json::array();
    for (std::size_t index = 0; index < telemetry->joint_diagnostics.size(); ++index) {
      const auto& source = telemetry->joint_diagnostics[index];
      nlohmann::json joint{{"index", index}};
      if (index < device->part().joint_names.size())
        joint["name"] = device->part().joint_names[index];
      if (source.enabled) joint["enabled"] = *source.enabled;
      if (source.error_code) joint["error_code"] = *source.error_code;
      if (source.motor_current_raw) joint["motor_current_raw"] = *source.motor_current_raw;
      if (source.temperature_raw) joint["temperature_raw"] = *source.temperature_raw;
      if (source.voltage_raw) joint["voltage_raw"] = *source.voltage_raw;
      diagnostics.push_back(std::move(joint));
    }
    snapshot["joint_diagnostics"] = std::move(diagnostics);
  }
  return snapshot;
}

}  // namespace eos::runtime::perception::body

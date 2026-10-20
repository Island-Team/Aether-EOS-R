#include "detail.h"
#include "reader.h"

namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildGripper(const std::string& part_id) {
  const hardware_runtime::Device* device =
      FindDevice(registry_, DeviceKind::kGripper, "gripper", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("gripper", part_id);
  }
  return DeviceSummaryOf(*device, "gripper");
}

nlohmann::json Reader::BuildGripperStatus(const std::string& robot_id) {
  const hardware_runtime::Device* device =
      FindRobotDevice(registry_, robot_id, DeviceKind::kGripper);
  if (device == nullptr) {
    ThrowAbsentPart("gripper", robot_id);
  }

  const DeviceStatus status = ProjectDeviceStatus(*device);
  nlohmann::json snapshot = StartEnvelope("gripper.status", "tool", device->identity().device_id,
                                          &device->part(), "gripper/" + robot_id + "/status");
  snapshot["state"] = status.state;
  snapshot["health"] = status.health;
  const auto telemetry = device->feeds().ReadGripperTelemetry();
  snapshot["telemetry_valid"] = telemetry.has_value();
  if (telemetry) {
    snapshot["jaw_state"] = JawOf(telemetry->state);
    if (telemetry->position_pct) snapshot["position_pct"] = *telemetry->position_pct;
    if (telemetry->force_n) snapshot["force_n"] = *telemetry->force_n;
  }
  return snapshot;
}

}  // namespace eos::runtime::perception::body

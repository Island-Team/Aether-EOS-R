#include "detail.h"
#include "reader.h"
#include "src/eos/runtime/models/camera_object.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/models/microphone_vad.h"

namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildDevices() {
  nlohmann::json robots = nlohmann::json::array();
  for (const hardware_runtime::RobotIdentity& robot : registry_.robots()) {
    std::size_t count = 0;
    for (const hardware_runtime::Device& device : registry_.devices()) {
      if (device.robot_id() == robot.id) ++count;
    }
    robots.push_back({
        {"robot_id", robot.id},
        {"robot_name", robot.name},
        {"model", robot.model.has_value() ? nlohmann::json(*robot.model) : nlohmann::json()},
        {"vendor", robot.vendor.has_value() ? nlohmann::json(*robot.vendor) : nlohmann::json()},
        {"description",
         robot.description.has_value() ? nlohmann::json(*robot.description) : nlohmann::json()},
        {"base_uri", "robots/" + robot.name},
        {"devices", count},
    });
  }

  nlohmann::json devices = nlohmann::json::array();
  for (const hardware_runtime::Device& device : registry_.devices()) {
    const char* device_class = SegmentOfKind(device.part().kind);
    if (device_class == nullptr) continue;
    nlohmann::json primitives = nlohmann::json::array();
    for (const hardware_runtime::PrimitiveDeclaration& primitive : device.part().primitives) {
      primitives.push_back(primitive.name);
    }
    const std::string device_id = device.identity().device_id;
    devices.push_back({
        {"id", device_id},
        {"class", device_class},
        {"robot", device.robot_id()},
        {"description", device.part().description.has_value()
                            ? nlohmann::json(*device.part().description)
                            : nlohmann::json()},
        {"tags", device.part().tags},
        {"primitives", std::move(primitives)},
    });
  }
  return {{"robots", std::move(robots)}, {"devices", std::move(devices)}};
}

nlohmann::json Reader::BuildDeviceState(const std::string& robot_id, const std::string& device_id) {
  const hardware_runtime::Device* device = registry_.FindDevice(device_id);
  if (device == nullptr || device->robot_id() != robot_id || !device->ReadHealth().admitted) {
    throw std::runtime_error("state projection: robot '" + robot_id + "' has no device '" +
                             device_id + "'");
  }
  if (device->part().kind == "base") return BuildBase(device_id);
  if (device->part().kind == "gimbal") return BuildGimbal(device_id);
  if (device->part().kind == "arm") return BuildManipulator(device_id);
  if (device->part().kind == "cam") return BuildCamera(device_id);
  if (device->part().kind == "mic") return BuildMic(device_id);
  if (device->part().kind == "gripper") return BuildGripper(device_id);
  if (device->part().kind == "speaker") return BuildSpeaker(device_id);
  throw std::runtime_error("state projection: device '" + device_id + "' has unsupported class '" +
                           device->part().kind + "'");
}

nlohmann::json Reader::ListResources() {
  nlohmann::json listing = nlohmann::json::array();
  for (const hardware_runtime::RobotIdentity& robot : registry_.robots()) {
    for (const hardware_runtime::Device& device : registry_.devices()) {
      const char* device_class = SegmentOfKind(device.part().kind);
      if (device.robot_id() != robot.id || device_class == nullptr || !device.ReadHealth().admitted)
        continue;
      listing.push_back(
          {{"class", device_class},
           {"uri", "robots/" + robot.name + "/devices/" + device.identity().device_id + "/state"}});
    }
    if (camera_pose_ && camera_pose_->Latest())
      listing.push_back(
          {{"class", "detection"}, {"uri", "robots/" + robot.name + "/detection/state"}});
    if (camera_object_) {
      for (const auto& device : registry_.devices()) {
        if (device.robot_id() != robot.id || device.identity().kind != DeviceKind::kCamera ||
            !camera_object_->Latest(device.identity().device_id)) continue;
        listing.push_back({{"class", "detection"},
                           {"uri", "robots/" + robot.name + "/cameras/" +
                                       device.identity().device_id + "/detections/state"}});
        if (device.identity().device_id == default_object_camera_device_id_ ||
            (default_object_camera_device_id_.empty() && device.part().part_id == "head"))
          listing.push_back({{"class", "detection"},
                             {"uri", "robots/" + robot.name + "/detection/head/state"}});
      }
    }
    if (microphone_vad_ && microphone_vad_->Latest()) {
      listing.push_back({{"class", "vad"}, {"uri", "robots/" + robot.name + "/world/vad/state"}});
      if (!microphone_vad_->Latest()->asr_model_id.empty() &&
          !microphone_vad_->Latest()->asr_response.is_null())
        listing.push_back({{"class", "asr"}, {"uri", "robots/" + robot.name + "/world/asr/state"}});
    }
    const hardware_runtime::Device* arm =
        FindRobotDevice(registry_, robot.id, DeviceKind::kManipulator);
    if (arm && arm->feeds().ReadManipulatorTelemetry())
      listing.push_back({{"class", "arm"}, {"uri", "robots/" + robot.name + "/arm/state"}});
    for (const auto& status :
         {std::pair{DeviceKind::kBase, "base"}, std::pair{DeviceKind::kManipulator, "arm"},
          std::pair{DeviceKind::kGripper, "gripper"}}) {
      const hardware_runtime::Device* device = FindRobotDevice(registry_, robot.id, status.first);
      if (device != nullptr && device->ReadHealth().admitted) {
        listing.push_back({{"class", status.second},
                           {"uri", "robots/" + robot.name + "/" + status.second + "/status"}});
      }
    }
  }
  return listing;
}

nlohmann::json Reader::BuildRobotOverviews() {
  nlohmann::json robots = nlohmann::json::array();
  for (const hardware_runtime::RobotIdentity& robot : registry_.robots()) {
    std::size_t total = 0;
    std::size_t alive = 0;
    std::size_t admitted = 0;
    nlohmann::json faults = nlohmann::json::array();
    for (const hardware_runtime::Device& device : registry_.devices()) {
      if (device.robot_id() != robot.id) continue;
      ++total;
      const hardware_runtime::DeviceHealth health = device.ReadHealth();
      if (health.alive) {
        ++alive;
      } else {
        faults.push_back("device '" + device.identity().device_id + "' halted");
      }
      if (health.admitted) ++admitted;
    }
    const bool estop = registry_.EstopLatched();
    const bool ready = total > 0 && total == alive && total == admitted && !estop;
    robots.push_back({
        {"robot_id", robot.id},
        {"robot_name", robot.name},
        {"model", robot.model.has_value() ? nlohmann::json(*robot.model) : nlohmann::json()},
        {"base_uri", "robots/" + robot.name},
        {"status", ready ? "ready" : "degraded"},
        {"estop", estop},
        {"devices_total", total},
        {"devices_alive", alive},
        {"devices_admitted", admitted},
        {"faults", std::move(faults)},
    });
  }
  return robots;
}

}  // namespace eos::runtime::perception::body

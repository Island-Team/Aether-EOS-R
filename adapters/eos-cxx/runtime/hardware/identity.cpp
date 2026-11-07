#include <stdexcept>
#include <utility>

#include "registry.h"

namespace eos::runtime::hardware_runtime {
namespace {
const char* KindName(hardware::DeviceKind kind) {
  switch (kind) {
    case hardware::DeviceKind::kManipulator:
      return "manipulator";
    case hardware::DeviceKind::kBase:
      return "base";
    case hardware::DeviceKind::kGimbal:
      return "gimbal";
    case hardware::DeviceKind::kCamera:
      return "camera";
    case hardware::DeviceKind::kMicrophone:
      return "microphone";
    case hardware::DeviceKind::kGripper:
      return "gripper";
    case hardware::DeviceKind::kSpeaker:
      return "speaker";
  }
  return "unknown";
}
}  // namespace

Device::Device(std::unique_ptr<hardware::Backend> backend, std::string robot_id, PartConfig part,
               hardware::FeedDeclarations envelope)
    : backend_(std::move(backend)),
      robot_id_(std::move(robot_id)),
      part_(std::move(part)),
      envelope_(envelope) {}

const RobotIdentity* Registry::FindRobot(const std::string& robot_id) const {
  for (const RobotIdentity& robot : robots_)
    if (robot.id == robot_id || robot.name == robot_id) return &robot;
  return nullptr;
}

Device* Registry::FindDeviceForRelease(const std::string& device_id) {
  for (Device& device : devices_)
    if (device.identity().device_id == device_id) return &device;
  return nullptr;
}

const Device* Registry::FindDevice(const std::string& device_id) const {
  for (const Device& device : devices_)
    if (device.identity().device_id == device_id) return &device;
  return nullptr;
}

const Device& Registry::Find(hardware::DeviceKind kind) const {
  for (const Device& device : devices_)
    if (device.identity().kind == kind) return device;
  throw std::runtime_error(std::string("registry: no device of kind '") + KindName(kind) + "'");
}

const Device& Registry::Find(hardware::DeviceKind kind, const std::string& part_id) const {
  for (const Device& device : devices_)
    if (device.identity().kind == kind && device.part().part_id == part_id) return device;
  throw std::runtime_error(std::string("registry: no device of kind '") + KindName(kind) +
                           "' with part '" + part_id + "'");
}
}  // namespace eos::runtime::hardware_runtime

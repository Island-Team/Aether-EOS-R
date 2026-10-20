#include <algorithm>
#include <cmath>

#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/arm/demo_arm.h"
#endif
#if defined(EOS_BACKEND_ROS2)
#include "src/eos/ext/backends/ros2/ros2_backend.h"
#endif

namespace eos::runtime::hardware_runtime::detail {
namespace {
void RequireKeys(const toml::table& table, const std::string& path, const std::string& where) {
  for (const auto& [key, value] : table) {
    (void)value;
    const std::string name(key.str());
    if (name != "link_lengths_m" && name != "mount_x_m" && name != "mount_y_m" &&
        name != "initial_joint_positions_rad")
      Fail(path, where + ": unknown key '" + name + "' for kind 'arm'");
  }
}
}  // namespace

void ApplyArmDefaults(PartConfig& config) { config.arm = ArmPartConfig{}; }

void ParseArmPart(const toml::table& table, PartConfig& config, const std::string& path,
                  const std::string& where) {
  RequireKeys(table, path, where);
  ArmPartConfig& arm = *config.arm;
  if (const auto* node = table.get("link_lengths_m"); node != nullptr) {
    const auto* array = node->as_array();
    if (array == nullptr) Fail(path, where + ": key 'link_lengths_m' must be an array of numbers");
    if (array->size() > arm.link_lengths_m.size())
      Fail(path, where + ": key 'link_lengths_m' must hold at most " +
                     std::to_string(arm.link_lengths_m.size()) + " entries");
    for (auto& slot : arm.link_lengths_m) slot = std::nullopt;
    std::size_t slot = 0;
    for (const auto& entry : *array) {
      const auto value = entry.value<double>();
      if (!value.has_value())
        Fail(path, where + ": key 'link_lengths_m' must contain only numbers");
      if (*value < 0.0)
        Fail(path, where + ": key 'link_lengths_m' must not contain negative numbers");
      arm.link_lengths_m[slot++] = *value;
    }
  }
  if (const auto* node = table.get("initial_joint_positions_rad"); node != nullptr) {
    const auto* array = node->as_array();
    if (array == nullptr || array->empty() || array->size() > 6)
      Fail(path, where + ": key 'initial_joint_positions_rad' must hold 1..6 numbers");
    for (const auto& entry : *array) {
      const auto value = entry.value<double>();
      if (!value.has_value() || !std::isfinite(*value))
        Fail(path, where + ": key 'initial_joint_positions_rad' must contain only finite numbers");
      arm.initial_joint_positions_rad.push_back(*value);
    }
  }
  arm.mount_x_m = OptionalCoordinate(table, "mount_x_m", arm.mount_x_m, path, where);
  arm.mount_y_m = OptionalCoordinate(table, "mount_y_m", arm.mount_y_m, path, where);
}

void ValidateArm(const DeviceBinding& device, const std::string& path, const std::string& where) {
  const bool initializes = std::any_of(
      device.part.primitives.begin(), device.part.primitives.end(),
      [](const PrimitiveDeclaration& primitive) { return primitive.name == "arm.init"; });
  const std::size_t joints = device.part.joint_names.empty() ? 6 : device.part.joint_names.size();
  if (initializes && (!device.part.arm.has_value() ||
                      device.part.arm->initial_joint_positions_rad.size() != joints))
    Fail(path, where +
                   ": primitive 'arm.init' requires [device.part].initial_joint_positions_rad "
                   "with one value for every configured joint");
}

std::unique_ptr<hardware::Backend> MakeArmBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo") {
    const ArmPartConfig geometry = part.arm.value_or(ArmPartConfig{});
    return std::make_unique<backends::demo::DemoArm>(
        std::move(device_id), std::move(simulator_world), geometry.link_lengths_m[0].value_or(0.0),
        geometry.link_lengths_m[1].value_or(0.0), geometry.mount_x_m, geometry.mount_y_m,
        part.joint_names.empty() ? 6 : part.joint_names.size());
  }
#endif
#if defined(EOS_BACKEND_ROS2)
  if (type == "ros2") {
    if (!part.ros2.has_value())
      throw std::runtime_error("ROS 2 arm binding has no ROS 2 configuration");
    return std::make_unique<backends::ros2::Ros2Backend>(
        std::move(device_id), *part.ros2, hardware::DeviceKind::kManipulator, part.joint_names);
  }
#endif
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

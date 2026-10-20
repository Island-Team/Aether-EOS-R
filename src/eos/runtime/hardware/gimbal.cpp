#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/gimbal/demo_gimbal.h"
#endif
#if defined(EOS_BACKEND_ROS2)
#include "src/eos/ext/backends/ros2/ros2_backend.h"
#endif

namespace eos::runtime::hardware_runtime::detail {
void ApplyGimbalDefaults(PartConfig& config) { config.gimbal = GimbalPartConfig{}; }
void ParseGimbalPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where) {
  for (const auto& [key, value] : table) {
    (void)value;
    const std::string name(key.str());
    if (name != "fov_rad" && name != "range_m")
      Fail(path, where + ": unknown key '" + name + "' for kind 'gimbal'");
  }
  GimbalPartConfig& gimbal = *config.gimbal;
  gimbal.fov_rad = OptionalLength(table, "fov_rad", gimbal.fov_rad, path, where);
  gimbal.range_m = OptionalLength(table, "range_m", gimbal.range_m, path, where);
}
std::unique_ptr<hardware::Backend> MakeGimbalBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo") {
    return std::make_unique<backends::demo::DemoGimbal>(std::move(device_id),
                                                        std::move(simulator_world));
  }
#endif
#if defined(EOS_BACKEND_ROS2)
  if (type == "ros2") {
    if (!part.ros2.has_value())
      throw std::runtime_error("ROS 2 gimbal binding has no ROS 2 configuration");
    return std::make_unique<backends::ros2::Ros2Backend>(
        std::move(device_id), *part.ros2, hardware::DeviceKind::kGimbal, part.joint_names);
  }
#endif
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

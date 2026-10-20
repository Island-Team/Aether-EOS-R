#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/gripper/demo_gripper.h"
#endif
#if defined(EOS_BACKEND_ROS2)
#include "src/eos/ext/backends/ros2/ros2_backend.h"
#endif

namespace eos::runtime::hardware_runtime::detail {
void ParseGripperPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where) {
  RequireNoPartKeys(table, config, path, where);
}
std::unique_ptr<hardware::Backend> MakeGripperBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo")
    return std::make_unique<backends::demo::DemoGripper>(std::move(device_id),
                                                         std::move(simulator_world));
#endif
#if defined(EOS_BACKEND_ROS2)
  if (type == "ros2") {
    if (!part.ros2.has_value())
      throw std::runtime_error("ROS 2 gripper binding has no ROS 2 configuration");
    return std::make_unique<backends::ros2::Ros2Backend>(std::move(device_id), *part.ros2,
                                                         hardware::DeviceKind::kGripper);
  }
#endif
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

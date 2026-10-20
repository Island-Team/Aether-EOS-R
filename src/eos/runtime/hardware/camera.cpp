#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/cam/demo_cam.h"
#endif
#include "src/eos/ext/backends/v4l2/v4l2_cam.h"
#if defined(EOS_BACKEND_ROS2)
#include "src/eos/ext/backends/ros2/ros2_backend.h"
#endif

namespace eos::runtime::hardware_runtime::detail {
void ApplyCameraDefaults(PartConfig& config) { config.camera = CameraPartConfig{}; }
void ParseCameraPart(const toml::table& table, PartConfig& config, const std::string& path,
                     const std::string& where) {
  for (const auto& [key, value] : table) {
    (void)value;
    if (key != "fov_rad")
      Fail(path, where + ": unknown key '" + std::string(key.str()) + "' for kind 'cam'");
  }
  CameraPartConfig& camera = *config.camera;
  camera.fov_rad = OptionalLength(table, "fov_rad", camera.fov_rad.value_or(1.4), path, where);
}
std::unique_ptr<hardware::Backend> MakeCameraBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo")
    return std::make_unique<backends::demo::DemoCam>(std::move(device_id),
                                                     std::move(simulator_world));
#endif
  if (type == "v4l2") {
    backends::v4l2::V4l2CamConfig config;
    config.device_id = std::move(device_id);
    config.video_device = part.v4l2->video_device;
    config.frame_id = part.v4l2->frame_id;
    config.width_px = part.v4l2->width_px;
    config.height_px = part.v4l2->height_px;
    config.fps = part.v4l2->fps;
    config.read_timeout_ms = part.v4l2->read_timeout_ms;
    config.max_frame_bytes = part.v4l2->max_frame_bytes;
    return std::make_unique<backends::v4l2::V4l2Cam>(std::move(config));
  }
#if defined(EOS_BACKEND_ROS2)
  if (type == "ros2") {
    if (!part.ros2.has_value())
      throw std::runtime_error("ROS 2 camera binding has no ROS 2 configuration");
    return std::make_unique<backends::ros2::Ros2Backend>(std::move(device_id), *part.ros2,
                                                         hardware::DeviceKind::kCamera);
  }
#endif
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

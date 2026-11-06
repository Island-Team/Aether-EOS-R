#include "class.h"

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
}  // namespace eos::runtime::hardware_runtime::detail

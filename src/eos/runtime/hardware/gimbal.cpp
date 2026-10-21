#include "class.h"

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
}  // namespace eos::runtime::hardware_runtime::detail

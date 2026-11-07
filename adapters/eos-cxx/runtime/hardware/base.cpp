#include "class.h"

namespace eos::runtime::hardware_runtime::detail {
void ApplyBaseDefaults(PartConfig& config) { config.base = BasePartConfig{}; }
void ParseBasePart(const toml::table& table, PartConfig& config, const std::string& path,
                   const std::string& where) {
  for (const auto& [key, value] : table) {
    (void)value;
    const std::string name(key.str());
    if (name != "footprint_length_m" && name != "footprint_width_m")
      Fail(path, where + ": unknown key '" + name + "' for kind 'base'");
  }
  BasePartConfig& base = *config.base;
  base.footprint_length_m =
      OptionalLength(table, "footprint_length_m", base.footprint_length_m, path, where);
  base.footprint_width_m =
      OptionalLength(table, "footprint_width_m", base.footprint_width_m, path, where);
}
}  // namespace eos::runtime::hardware_runtime::detail

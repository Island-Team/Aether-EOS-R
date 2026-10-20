#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/base/demo_base.h"
#endif

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
std::unique_ptr<hardware::Backend> MakeBaseBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo") {
    const BasePartConfig config = part.base.value_or(BasePartConfig{});
    return std::make_unique<backends::demo::DemoBase>(
        std::move(device_id), std::move(simulator_world), config.footprint_width_m,
        config.footprint_length_m);
  }
#endif
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

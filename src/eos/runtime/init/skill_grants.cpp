#include "src/eos/runtime/init/stack_manager.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "src/eos/runtime/hardware/manager/capability_bindings.h"

namespace eos::runtime::init {

std::vector<skills::worker::Grant> ResolveSkillGrants(
    const std::vector<SkillCapability>& capabilities,
    const hardware_runtime::Registry& registry, const skills::Metadata& metadata,
    const nlohmann::json& arguments, const std::string& execution_id) {
  std::vector<skills::worker::Grant> grants;
  for (const auto& capability : capabilities) {
    if (capability.skill != metadata.name || capability.skill_version != metadata.version)
      continue;
    const bool declared = std::any_of(metadata.primitives.begin(), metadata.primitives.end(),
        [&](const auto& p) {
          return p.name == capability.primitive && p.version == capability.primitive_version;
        });
    if (!declared)
      throw std::invalid_argument("skill capability names primitive not declared by skill: " +
                                  capability.slot);
    const nlohmann::json::json_pointer pointer(capability.argument_pointer);
    if (!arguments.contains(pointer) || !arguments.at(pointer).is_string())
      throw std::invalid_argument("skill capability selector must be a string at " +
                                  capability.argument_pointer);
    const std::string selector = arguments.at(pointer).get<std::string>();
    if (selector.empty() || selector.size() > 128)
      throw std::invalid_argument("skill capability selector is invalid");
    const hardware_runtime::manager::CapabilitySlot slot{
        capability.slot, capability.robot_id, capability.device_class, selector,
        capability.primitive, capability.primitive_version};
    const auto resolved = hardware_runtime::manager::ResolveCapabilitySlots(registry, {slot});
    if (resolved.size() != 1)
      throw std::runtime_error("skill capability slot did not resolve uniquely");
    grants.push_back({execution_id, 1, capability.primitive, capability.primitive_version,
                      resolved.front().device_id,
                      std::chrono::steady_clock::now() + std::chrono::minutes(10), selector});
  }
  return grants;
}

}  // namespace eos::runtime::init

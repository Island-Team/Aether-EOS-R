#include "capability_bindings.h"

#include <set>
#include <stdexcept>
#include <utility>

namespace eos::runtime::hardware_runtime::manager {

std::vector<ResolvedCapability> ResolveCapabilitySlots(
    const Registry& registry, const std::vector<CapabilitySlot>& slots) {
  std::set<std::string> names;
  std::vector<ResolvedCapability> resolved;
  resolved.reserve(slots.size());
  for (const auto& slot : slots) {
    if (slot.name.empty() || slot.robot_id.empty() || slot.device_class.empty() ||
        slot.part_id.empty() || slot.primitive.empty() || slot.version.empty())
      throw std::invalid_argument("capability slot has an empty required field");
    if (!names.insert(slot.name).second)
      throw std::invalid_argument("duplicate capability slot: " + slot.name);
    if (slot.robot_id != registry.robot().id)
      throw std::invalid_argument("capability slot " + slot.name + " targets another robot");

    const Device* selected = nullptr;
    for (const Device& device : registry.devices()) {
      if (device.part().kind != slot.device_class || device.part().part_id != slot.part_id)
        continue;
      if (selected != nullptr)
        throw std::invalid_argument("capability slot " + slot.name + " is ambiguous");
      selected = &device;
    }
    if (selected == nullptr)
      throw std::invalid_argument("capability slot " + slot.name + " has no admitted device");
    bool primitive_admitted = false;
    for (const auto& declaration : selected->part().primitives)
      primitive_admitted |= declaration.name == slot.primitive && declaration.version == slot.version;
    if (!primitive_admitted)
      throw std::invalid_argument("capability slot " + slot.name + " names an unadmitted primitive");
    resolved.push_back({slot.name, selected->identity().device_id, slot.primitive, slot.version});
  }
  return resolved;
}

}  // namespace eos::runtime::hardware_runtime::manager

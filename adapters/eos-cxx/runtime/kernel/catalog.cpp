#include "catalog.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

namespace {
std::string Escape(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string escaped;
  for (unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.') {
      escaped += static_cast<char>(ch);
    } else {
      escaped += '%';
      escaped += hex[ch >> 4];
      escaped += hex[ch & 15];
    }
  }
  return escaped;
}
}  // namespace

namespace eos::runtime::kernel {

Catalog::Catalog(const hardware_runtime::Registry& registry,
                 const std::vector<hardware_runtime::manager::CapabilitySlot>& slots)
    : registry_(registry) {
  std::set<std::string> names;
  for (const auto& device : registry.devices()) {
    for (const auto& primitive : device.part().primitives) {
      auto name = HardwareName(registry.robot().id, device.part().kind,
                               device.part().part_id, primitive.name, primitive.version);
      if (!names.insert(name).second)
        throw std::invalid_argument("ambiguous canonical hardware capability: " + name);
      capabilities_.push_back({std::move(name), device.identity().device_id,
                               primitive.name, primitive.version});
    }
    // Controlled stop is implicit for every admitted motion device, including
    // configurations that cannot declare privileged primitives.
    if (device.identity().kind == hardware::DeviceKind::kBase ||
        device.identity().kind == hardware::DeviceKind::kManipulator ||
        device.identity().kind == hardware::DeviceKind::kGimbal) {
      auto name = HardwareName(registry.robot().id, device.part().kind,
                               device.part().part_id, "motion.stop", "1.0.0");
      if (!names.insert(name).second)
        throw std::invalid_argument("ambiguous canonical hardware capability: " + name);
      capabilities_.push_back({std::move(name), device.identity().device_id,
                               "motion.stop", "1.0.0"});
    }
  }
  // These commands have no device target and must remain reachable even when
  // ordinary devices have failed. Dispatcher owns their safety semantics.
  for (const char* primitive : {"robot.stop", "safety.estop"}) {
    auto name = "runtime/" + Escape(registry.robot().id) + '/' + primitive + "@1.0.0";
    names.insert(name);
    capabilities_.push_back({std::move(name), "", primitive, "1.0.0"});
  }
  for (auto capability : hardware_runtime::manager::ResolveCapabilitySlots(registry, slots)) {
    if (!names.insert(capability.name).second)
      throw std::invalid_argument("duplicate or reserved capability name: " + capability.name);
    capabilities_.push_back(std::move(capability));
  }
}

std::string Catalog::HardwareName(const std::string& robot_id, const std::string& device_class,
                                  const std::string& part_id, const std::string& primitive,
                                  const std::string& version) {
  return "hardware/" + Escape(robot_id) + '/' + Escape(device_class) + '/' + Escape(part_id) +
         '/' + Escape(primitive) + '@' + Escape(version);
}

const hardware_runtime::manager::ResolvedCapability* Catalog::Find(
    const std::string& name) const {
  const auto it = std::find_if(capabilities_.begin(), capabilities_.end(),
                               [&](const auto& capability) { return capability.name == name; });
  return it == capabilities_.end() ? nullptr : &*it;
}

const hardware_runtime::manager::ResolvedCapability* Catalog::FindHardware(
    const std::string& primitive, const std::string& version,
    const std::string& device_id) const {
  for (const auto& capability : capabilities_)
    if ((capability.name.rfind("hardware/", 0) == 0 ||
         capability.name.rfind("runtime/", 0) == 0) && capability.primitive == primitive &&
        capability.version == version && capability.device_id == device_id)
      return &capability;
  return nullptr;
}

bool Catalog::Available(const std::string& name) const {
  const auto* capability = Find(name);
  if (capability == nullptr) return false;
  if (capability->primitive == "safety.estop" || capability->primitive == "robot.stop" ||
      capability->primitive == "motion.stop") return true;
  const auto* device = registry_.FindDevice(capability->device_id);
  if (device == nullptr) return false;
  const auto health = device->ReadHealth();
  return health.alive && health.admitted;
}

}  // namespace eos::runtime::kernel

#include "common.h"

#include <algorithm>
#include <cmath>

namespace eos::runtime::dispatch::detail {
namespace {
bool Number(const nlohmann::json& value, const char* key, double* out) {
  const auto it = value.find(key);
  if (it == value.end() || !it->is_number()) return false;
  *out = it->get<double>();
  return std::isfinite(*out);
}
}  // namespace

const hardware_runtime::NumericRange* LimitOf(const hardware_runtime::Device& device, const std::string& primitive,
                                    const char* argument) {
  for (const hardware_runtime::PrimitiveDeclaration& declaration : device.part().primitives) {
    if (declaration.name != primitive) continue;
    const auto found = declaration.limits.find(argument);
    return found == declaration.limits.end() ? nullptr : &found->second;
  }
  return nullptr;
}

bool InAdmittedRange(const nlohmann::json& arguments, const char* key, const hardware_runtime::Device& device,
                     const std::string& primitive, double* out) {
  const hardware_runtime::NumericRange* range = LimitOf(device, primitive, key);
  return range != nullptr && Number(arguments, key, out) && range->Contains(*out);
}

bool OptionalInAdmittedRange(const nlohmann::json& arguments, const char* key,
                             const hardware_runtime::Device& device, const std::string& primitive,
                             std::optional<double>* value) {
  if (!arguments.contains(key)) return true;
  double parsed = 0.0;
  if (!InAdmittedRange(arguments, key, device, primitive, &parsed)) return false;
  *value = parsed;
  return true;
}

bool IsMotion(hardware::DeviceKind kind) {
  return kind == hardware::DeviceKind::kBase || kind == hardware::DeviceKind::kManipulator ||
         kind == hardware::DeviceKind::kGimbal;
}

hardware::DeviceKind KindOf(const std::string& device_class) {
  if (device_class == "base") return hardware::DeviceKind::kBase;
  if (device_class == "gimbal") return hardware::DeviceKind::kGimbal;
  if (device_class == "gripper") return hardware::DeviceKind::kGripper;
  if (device_class == "speaker") return hardware::DeviceKind::kSpeaker;
  if (device_class == "cam") return hardware::DeviceKind::kCamera;
  return hardware::DeviceKind::kManipulator;
}

const hardware_runtime::PrimitiveDeclaration* DeclarationOf(const hardware_runtime::Device& device,
                                                  const std::string& primitive) {
  const auto found = std::find_if(
      device.part().primitives.begin(), device.part().primitives.end(),
      [&](const hardware_runtime::PrimitiveDeclaration& item) { return item.name == primitive; });
  return found == device.part().primitives.end() ? nullptr : &*found;
}

bool SupportsReference(const hardware_runtime::PrimitiveDeclaration& declaration,
                       const std::string& reference) {
  return std::find(declaration.references.begin(), declaration.references.end(), reference) !=
         declaration.references.end();
}
}  // namespace eos::runtime::dispatch::detail

#pragma once

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "src/eos/runtime/hardware/registry.h"

namespace eos::runtime::dispatch::detail {
inline constexpr const char* kInvalidArgument = "rejected.invalid_arguments";
inline constexpr const char* kInvalidTarget = "rejected.invalid_target";
inline constexpr const char* kInvalidState = "rejected.invalid_state";
inline constexpr const char* kBusy = "rejected.busy";
inline constexpr const char* kUnsupported = "rejected.unsupported";
inline constexpr const char* kUnsupportedReference = "rejected.unsupported_reference";
inline constexpr const char* kUnavailableDependency = "rejected.unavailable_dependency";
inline constexpr const char* kEstop = "rejected.estop";
inline constexpr const char* kInternal = "rejected.internal";

const hardware_runtime::NumericRange* LimitOf(const hardware_runtime::Device& device,
                                              const std::string& primitive, const char* argument);
bool InAdmittedRange(const nlohmann::json& arguments, const char* key,
                     const hardware_runtime::Device& device, const std::string& primitive,
                     double* out);
bool OptionalInAdmittedRange(const nlohmann::json& arguments, const char* key,
                             const hardware_runtime::Device& device, const std::string& primitive,
                             std::optional<double>* value);
bool IsMotion(hardware::DeviceKind kind);
hardware::DeviceKind KindOf(const std::string& device_class);
const hardware_runtime::PrimitiveDeclaration* DeclarationOf(const hardware_runtime::Device& device,
                                                            const std::string& primitive);
bool SupportsReference(const hardware_runtime::PrimitiveDeclaration& declaration,
                       const std::string& reference);
std::optional<std::array<double, 3>> ResolveBaseStreamMapTarget(const hardware::BaseTelemetry& pose,
                                                                bool absolute, double x_m,
                                                                double y_m, double yaw_rad);
}  // namespace eos::runtime::dispatch::detail

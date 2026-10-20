#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "src/eos/runtime/hardware/registry.h"

namespace eos::runtime::perception::body::detail {
using hardware::DeviceKind;
using hardware_runtime::PartConfig;

/// Signals that a valid resource has no producer sample yet. Routers may
/// treat this expected condition differently from projection failures.
class ResourceUnavailable : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

const hardware_runtime::Device* FindDevice(const hardware_runtime::Registry& registry,
                                           hardware::DeviceKind kind, const char* binding_kind,
                                           const std::string& part_id);
const hardware_runtime::Device* FindRobotDevice(const hardware_runtime::Registry& registry,
                                                const std::string& robot_id,
                                                hardware::DeviceKind kind);
/// Returns the compact common state/health projection for one admitted device.
nlohmann::json DeviceSummaryOf(const hardware_runtime::Device& device, const char* device_class);
[[noreturn]] void ThrowAbsentPart(const char* resource, const std::string& part_id);
std::string Base64Encode(const std::uint8_t* bytes, std::size_t size);
nlohmann::json BaseStateOf(const hardware_runtime::PartConfig& part,
                           const hardware::BaseTelemetry& telemetry);
nlohmann::json GimbalStateOf(const hardware_runtime::PartConfig& part,
                             const hardware::GimbalTelemetry& telemetry);
nlohmann::json ManipulatorStateOf(const hardware_runtime::PartConfig& part,
                                  const hardware::ManipulatorTelemetry& telemetry);
std::vector<std::string> SplitUri(const std::string& text);
std::string TimestampOf(std::chrono::system_clock::time_point time);
const char* JawOf(hardware::GripperState state);
const char* PostureOf(hardware::BasePosture posture);
}  // namespace eos::runtime::perception::body::detail

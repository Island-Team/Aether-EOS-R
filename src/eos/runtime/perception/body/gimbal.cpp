#include "detail.h"
#include "reader.h"
namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildGimbal(const std::string& part_id) {
  const hardware_runtime::Device* device = FindDevice(registry_, DeviceKind::kGimbal, "gimbal", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("gimbal", part_id);
  }
  nlohmann::json state = DeviceSummaryOf(*device, "gimbal");
  if (const auto telemetry = device->feeds().ReadGimbalTelemetry()) {
    state["pan_rad"] = telemetry->pan_rad;
    state["tilt_rad"] = telemetry->tilt_rad;
    state["pan_rate_rad_s"] = telemetry->pan_rate_rad_s;
    state["tilt_rate_rad_s"] = telemetry->tilt_rate_rad_s;
  }
  return state;
}

}  // namespace eos::runtime::perception::body

#include "detail.h"
#include "reader.h"
namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildCamera(const std::string& part_id) {
  const hardware_runtime::Device* device = FindDevice(registry_, DeviceKind::kCamera, "cam", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("camera", part_id);
  }
  return DeviceSummaryOf(*device, "camera");
}

}  // namespace eos::runtime::perception::body

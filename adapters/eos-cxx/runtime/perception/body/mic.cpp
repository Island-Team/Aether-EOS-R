#include "detail.h"
#include "reader.h"

namespace eos::runtime::perception::body {
using namespace detail;

nlohmann::json Reader::BuildMic(const std::string& part_id) {
  const hardware_runtime::Device* device = FindDevice(registry_, DeviceKind::kMicrophone, "mic", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("mic", part_id);
  }
  return DeviceSummaryOf(*device, "microphone");
}

}  // namespace eos::runtime::perception::body

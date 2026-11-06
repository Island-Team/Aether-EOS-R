#include "detail.h"
#include "reader.h"
namespace eos::runtime::perception::body {
using namespace detail;
nlohmann::json Reader::BuildSpeaker(const std::string& part_id) {
  const hardware_runtime::Device* device = FindDevice(registry_, DeviceKind::kSpeaker, "speaker", part_id);
  if (device == nullptr) {
    ThrowAbsentPart("speaker", part_id);
  }
  return DeviceSummaryOf(*device, "speaker");
}

}  // namespace eos::runtime::perception::body

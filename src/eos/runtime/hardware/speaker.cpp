#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/speaker/demo_speaker.h"
#endif
#include "src/eos/ext/backends/aplay/aplay_speaker.h"

namespace eos::runtime::hardware_runtime::detail {
void ParseSpeakerPart(const toml::table& table, PartConfig& config, const std::string& path,
                      const std::string& where) {
  RequireNoPartKeys(table, config, path, where);
}
std::unique_ptr<hardware::Backend> MakeSpeakerBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo")
    return std::make_unique<backends::demo::DemoSpeaker>(std::move(device_id),
                                                         std::move(simulator_world));
#endif
  if (type == "aplay") {
    if (!part.aplay.has_value()) {
      throw std::runtime_error("aplay speaker binding has no aplay configuration");
    }
    return std::make_unique<backends::aplay::AplaySpeaker>(
        backends::aplay::AplaySpeakerConfig{.device_id = std::move(device_id),
                                            .alsa_device = part.aplay->alsa_device,
                                            .max_stream_bytes = part.aplay->max_stream_bytes});
  }
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

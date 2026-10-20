#include "class.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/mic/demo_mic.h"
#endif
#include "src/eos/ext/backends/alsa/alsa_mic.h"

namespace eos::runtime::hardware_runtime::detail {
void ParseMicPart(const toml::table& table, PartConfig& config, const std::string& path,
                  const std::string& where) {
  RequireNoPartKeys(table, config, path, where);
}
std::unique_ptr<hardware::Backend> MakeMicBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
#if defined(EOS_BACKEND_DEMO)
  if (type == "demo") {
    return std::make_unique<backends::demo::DemoMic>(std::move(device_id),
                                                     std::move(simulator_world));
  }
#endif
  if (type == "alsa") {
    backends::alsa::AlsaMicConfig config;
    config.device_id = std::move(device_id);
    config.alsa_device = part.alsa->alsa_device;
    config.frame_id = part.alsa->frame_id;
    config.sample_rate_hz = part.alsa->sample_rate_hz;
    config.channels = part.alsa->channels;
    config.window_frames = part.alsa->window_frames;
    config.read_timeout_ms = part.alsa->read_timeout_ms;
    return std::make_unique<backends::alsa::AlsaMic>(std::move(config));
  }
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

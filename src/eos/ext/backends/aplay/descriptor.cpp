#include "descriptor.h"

#include <memory>
#include <utility>

#include "src/eos/ext/backends/detail/config_parser.h"
#include "src/eos/ext/backends/aplay/aplay_speaker.h"
#include "src/eos/ext/backends/aplay/config.h"
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
using namespace eos::backends::config;
AplayDeviceConfig ParseAplayTable(const toml::table& table, const std::string& path,
                                  const std::string& where) {
  static const char* const kAllowed[] = {"alsa_device", "max_stream_bytes", nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  AplayDeviceConfig config;
  if (const auto device = OptionalString(table, "alsa_device", path, where); device.has_value()) {
    config.alsa_device = *device;
  }
  if (config.alsa_device.empty()) Fail(path, where + ": key 'alsa_device' must not be empty");
  config.max_stream_bytes =
      OptionalBytes(table, "max_stream_bytes", config.max_stream_bytes, path, where);
  if (config.max_stream_bytes == 0) {
    Fail(path, where + ": key 'max_stream_bytes' must be positive");
  }
  return config;
}

struct AplaySettings final : BackendSettings {
  AplayDeviceConfig value;
};
BackendDescriptor AplayDescriptor() {
  BackendDescriptor d;
  d.type = "aplay";
  d.supports_class = [](std::string_view kind) { return kind == "speaker"; };
  d.parse = [](const toml::table& tbl, const PartConfig&, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    auto settings = std::make_shared<AplaySettings>();
    settings->value = ParseAplayTable(*OneTable(tbl, "aplay", true, path, where), path,
                                      where + " [device.backend.aplay]");
    return settings;
  };
  d.supports_primitive = [](const PrimitiveSpec& p) {
    return HasPrimitive(p, {"speaker.play", "speaker.speak"});
  };
  d.create = [](std::string id, const PartConfig&,
                const BackendSettings& settings) -> std::unique_ptr<hardware::Backend> {
    const auto& a = static_cast<const AplaySettings&>(settings).value;
    return std::make_unique<aplay::AplaySpeaker>(
        aplay::AplaySpeakerConfig{.device_id = std::move(id),
                                  .alsa_device = a.alsa_device,
                                  .max_stream_bytes = a.max_stream_bytes});
  };
  return d;
}

}  // namespace eos::backends

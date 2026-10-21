#include "descriptor.h"

#include <memory>
#include <utility>

#include "src/eos/ext/backends/detail/config_parser.h"
#include "src/eos/ext/backends/alsa/alsa_mic.h"
#include "src/eos/ext/backends/alsa/config.h"
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
using namespace eos::backends::config;
AlsaMicDeviceConfig ParseAlsaMicTable(const toml::table& table, const std::string& path,
                                      const std::string& where) {
  static const char* const kAllowed[] = {"alsa_device", "frame_id",      "sample_rate_hz",
                                         "channels",    "window_frames", "read_timeout_ms",
                                         nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  AlsaMicDeviceConfig config;
  if (const auto device = OptionalString(table, "alsa_device", path, where); device.has_value()) {
    config.alsa_device = *device;
  }
  if (config.alsa_device.empty()) Fail(path, where + ": key 'alsa_device' must not be empty");
  if (const auto frame = OptionalString(table, "frame_id", path, where); frame.has_value()) {
    config.frame_id = *frame;
  }
  if (config.frame_id.empty()) Fail(path, where + ": key 'frame_id' must not be empty");
  const std::uint64_t rate =
      OptionalBytes(table, "sample_rate_hz", config.sample_rate_hz, path, where);
  if (rate == 0 || rate > 192000) {
    Fail(path, where + ": key 'sample_rate_hz' must be within 1..192000");
  }
  config.sample_rate_hz = static_cast<std::uint32_t>(rate);
  const std::uint64_t channels = OptionalBytes(table, "channels", config.channels, path, where);
  if (channels == 0 || channels > 8) {
    Fail(path, where + ": key 'channels' must be within 1..8");
  }
  config.channels = static_cast<std::uint16_t>(channels);
  const std::uint64_t window_frames =
      OptionalBytes(table, "window_frames", config.window_frames, path, where);
  if (window_frames == 0 || window_frames > 48000) {
    Fail(path, where + ": key 'window_frames' must be within 1..48000");
  }
  config.window_frames = static_cast<std::uint32_t>(window_frames);
  const std::uint64_t read_timeout_ms =
      OptionalBytes(table, "read_timeout_ms", config.read_timeout_ms, path, where);
  if (read_timeout_ms == 0 || read_timeout_ms > 60000) {
    Fail(path, where + ": key 'read_timeout_ms' must be within 1..60000");
  }
  config.read_timeout_ms = static_cast<std::uint32_t>(read_timeout_ms);
  return config;
}

struct AlsaSettings final : BackendSettings {
  AlsaMicDeviceConfig value;
};
BackendDescriptor AlsaDescriptor() {
  BackendDescriptor d;
  d.type = "alsa";
  d.supports_class = [](std::string_view kind) { return kind == "mic"; };
  d.parse = [](const toml::table& tbl, const PartConfig&, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    auto settings = std::make_shared<AlsaSettings>();
    settings->value = ParseAlsaMicTable(*OneTable(tbl, "alsa", true, path, where), path,
                                        where + " [device.backend.alsa]");
    return settings;
  };
  d.supports_primitive = [](const PrimitiveSpec&) { return false; };
  d.create = [](std::string id, const PartConfig&,
                const BackendSettings& settings) -> std::unique_ptr<hardware::Backend> {
    const auto& a = static_cast<const AlsaSettings&>(settings).value;
    alsa::AlsaMicConfig config;
    config.device_id = std::move(id);
    config.alsa_device = a.alsa_device;
    config.frame_id = a.frame_id;
    config.sample_rate_hz = a.sample_rate_hz;
    config.channels = a.channels;
    config.window_frames = a.window_frames;
    config.read_timeout_ms = a.read_timeout_ms;
    return std::make_unique<alsa::AlsaMic>(std::move(config));
  };
  return d;
}

}  // namespace eos::backends

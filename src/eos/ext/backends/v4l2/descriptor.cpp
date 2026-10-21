#include "descriptor.h"

#include <memory>
#include <utility>

#include "src/eos/ext/backends/detail/config_parser.h"
#include "src/eos/ext/backends/v4l2/config.h"
#include "src/eos/ext/backends/v4l2/v4l2_cam.h"
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
using namespace eos::backends::config;
V4l2CamDeviceConfig ParseV4l2CamTable(const toml::table& table, const std::string& path,
                                      const std::string& where) {
  static const char* const kAllowed[] = {"video_device",    "frame_id", "width_px",
                                         "height_px",       "fps",      "read_timeout_ms",
                                         "max_frame_bytes", nullptr};
  RequireKnownKeys(table, kAllowed, path, where);
  V4l2CamDeviceConfig config;
  if (const auto device = OptionalString(table, "video_device", path, where); device.has_value()) {
    config.video_device = *device;
  }
  if (config.video_device.empty()) Fail(path, where + ": key 'video_device' must not be empty");
  if (const auto frame = OptionalString(table, "frame_id", path, where); frame.has_value()) {
    config.frame_id = *frame;
  }
  if (config.frame_id.empty()) Fail(path, where + ": key 'frame_id' must not be empty");
  const std::uint64_t width = OptionalBytes(table, "width_px", config.width_px, path, where);
  if (width == 0 || width > 16384) {
    Fail(path, where + ": key 'width_px' must be within 1..16384");
  }
  config.width_px = static_cast<std::uint32_t>(width);
  const std::uint64_t height = OptionalBytes(table, "height_px", config.height_px, path, where);
  if (height == 0 || height > 16384) {
    Fail(path, where + ": key 'height_px' must be within 1..16384");
  }
  config.height_px = static_cast<std::uint32_t>(height);
  const std::uint64_t fps = OptionalBytes(table, "fps", config.fps, path, where);
  if (fps == 0 || fps > 240) Fail(path, where + ": key 'fps' must be within 1..240");
  config.fps = static_cast<std::uint32_t>(fps);
  const std::uint64_t read_timeout_ms =
      OptionalBytes(table, "read_timeout_ms", config.read_timeout_ms, path, where);
  if (read_timeout_ms == 0 || read_timeout_ms > 60000) {
    Fail(path, where + ": key 'read_timeout_ms' must be within 1..60000");
  }
  config.read_timeout_ms = static_cast<std::uint32_t>(read_timeout_ms);
  config.max_frame_bytes =
      OptionalBytes(table, "max_frame_bytes", config.max_frame_bytes, path, where);
  if (config.max_frame_bytes == 0 || config.max_frame_bytes > 64 * 1024 * 1024) {
    Fail(path, where + ": key 'max_frame_bytes' must be within 1..67108864");
  }
  return config;
}

struct V4l2Settings final : BackendSettings {
  V4l2CamDeviceConfig value;
  std::uint64_t CameraFrameLimitBytes() const override { return value.max_frame_bytes; }
};
BackendDescriptor V4l2Descriptor() {
  BackendDescriptor d;
  d.type = "v4l2";
  d.supports_class = [](std::string_view kind) { return kind == "cam"; };
  d.parse = [](const toml::table& tbl, const PartConfig&, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    auto settings = std::make_shared<V4l2Settings>();
    settings->value = ParseV4l2CamTable(*OneTable(tbl, "v4l2", true, path, where), path,
                                        where + " [device.backend.v4l2]");
    return settings;
  };
  d.supports_primitive = [](const PrimitiveSpec& p) { return HasPrimitive(p, {"face.register"}); };
  d.create = [](std::string id, const PartConfig&,
                const BackendSettings& settings) -> std::unique_ptr<hardware::Backend> {
    const auto& a = static_cast<const V4l2Settings&>(settings).value;
    v4l2::V4l2CamConfig config;
    config.device_id = std::move(id);
    config.video_device = a.video_device;
    config.frame_id = a.frame_id;
    config.width_px = a.width_px;
    config.height_px = a.height_px;
    config.fps = a.fps;
    config.read_timeout_ms = a.read_timeout_ms;
    config.max_frame_bytes = a.max_frame_bytes;
    return std::make_unique<v4l2::V4l2Cam>(std::move(config));
  };
  return d;
}

}  // namespace eos::backends

#include "descriptor.h"

#include <arpa/inet.h>

#include <memory>
#include <utility>

#include "src/eos/runtime/hardware/detail/config_parser.h"
#include "src/eos/ext/backends/vendor/config.h"
#if defined(EOS_BACKEND_VENDOR)
#include "src/eos/ext/backends/vendor/vendor_base.h"
#include "src/eos/ext/backends/vendor/vendor_camera.h"
#endif
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
using namespace eos::backends::config;
VendorBaseDeviceConfig ParseVendorBaseTable(const toml::table& table, const std::string& path,
                                            const std::string& where) {
  static const char* const kAllowed[] = {"motion_host",
                                         "motion_port",
                                         "status_bind_host",
                                         "status_port",
                                         "expected_sender",
                                         "robotserver_host",
                                         "robotserver_port",
                                         "io_timeout_ms",
                                         "navigation_timeout_ms",
                                         "transition_timeout_ms",
                                         "idle_stand_ms",
                                         "map_id",
                                         "obstacle_host",
                                         "obstacle_port",
                                         "obstacle_timeout_ms",
                                         "obstacle_max_age_ms",
                                         "stream_axis_limit",
                                         "stream_position_tolerance_m",
                                         "stream_yaw_tolerance_rad",
                                         "stream_retarget_distance_m",
                                         "stream_retarget_yaw_rad",
                                         nullptr};
  RequireKnownKeys(table, kAllowed, path, where);

  VendorBaseDeviceConfig config;
  config.motion_host = RequireString(table, "motion_host", path, where);
  config.status_bind_host = RequireString(table, "status_bind_host", path, where);
  config.expected_sender = RequireString(table, "expected_sender", path, where);
  config.robotserver_host = RequireString(table, "robotserver_host", path, where);
  if (config.motion_host.empty() || config.status_bind_host.empty() ||
      config.expected_sender.empty() || config.robotserver_host.empty()) {
    Fail(path, where + ": host values must not be empty");
  }

  auto port = [&](const char* key) {
    const std::uint64_t value = RequireBytes(table, key, path, where);
    if (value == 0 || value > 65535) {
      Fail(path, where + ": key '" + key + "' must be within 1..65535");
    }
    return static_cast<std::uint16_t>(value);
  };
  auto timeout = [&](const char* key) {
    const std::uint64_t value = RequireBytes(table, key, path, where);
    if (value == 0 || value > 300000) {
      Fail(path, where + ": key '" + key + "' must be within 1..300000");
    }
    return value;
  };
  config.motion_port = port("motion_port");
  config.status_port = port("status_port");
  config.robotserver_port = port("robotserver_port");
  config.io_timeout_ms = timeout("io_timeout_ms");
  config.navigation_timeout_ms = timeout("navigation_timeout_ms");
  config.transition_timeout_ms = timeout("transition_timeout_ms");
  config.idle_stand_ms = timeout("idle_stand_ms");
  const std::uint64_t map_id = RequireBytes(table, "map_id", path, where);
  if (map_id > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
    Fail(path, where + ": key 'map_id' is too large");
  }
  config.map_id = static_cast<int>(map_id);
  if (table.get("obstacle_host") != nullptr) {
    config.obstacle_host = RequireString(table, "obstacle_host", path, where);
    in_addr address{};
    if (inet_pton(AF_INET, config.obstacle_host->c_str(), &address) != 1) {
      Fail(path, where + ": obstacle_host must be a numeric IPv4 address");
    }
    config.obstacle_port = port("obstacle_port");
    config.obstacle_timeout_ms = timeout("obstacle_timeout_ms");
    config.obstacle_max_age_ms = timeout("obstacle_max_age_ms");
  } else if (table.get("obstacle_port") != nullptr ||
             table.get("obstacle_timeout_ms") != nullptr ||
             table.get("obstacle_max_age_ms") != nullptr) {
    Fail(path, where + ": obstacle_host is required with obstacle endpoint settings");
  }
  if (table.get("stream_axis_limit") != nullptr) {
    const auto limit = RequireBytes(table, "stream_axis_limit", path, where);
    if (limit == 0 || limit > 32767) {
      Fail(path, where + ": key 'stream_axis_limit' must be within 1..32767");
    }
    config.stream_axis_limit = static_cast<int>(limit);
  }
  auto stream_tolerance = [&](const char* key, double* destination) {
    if (const auto* node = table.get(key)) {
      const auto value = node->value<double>();
      if (!value.has_value() || !std::isfinite(*value) || *value <= 0.0 || *value > 1.0) {
        Fail(path, where + ": key '" + key + "' must be within (0, 1]");
      }
      *destination = *value;
    }
  };
  stream_tolerance("stream_position_tolerance_m", &config.stream_position_tolerance_m);
  stream_tolerance("stream_yaw_tolerance_rad", &config.stream_yaw_tolerance_rad);
  stream_tolerance("stream_retarget_distance_m", &config.stream_retarget_distance_m);
  stream_tolerance("stream_retarget_yaw_rad", &config.stream_retarget_yaw_rad);
  if (config.stream_retarget_distance_m <= config.stream_position_tolerance_m ||
      config.stream_retarget_yaw_rad <= config.stream_yaw_tolerance_rad) {
    Fail(path, where + ": stream retarget thresholds must exceed arrival tolerances");
  }
  return config;
}

struct VendorSettings final : BackendSettings {
  std::optional<VendorBaseDeviceConfig> base;
};
BackendDescriptor VendorDescriptor() {
  BackendDescriptor d;
  d.type = "vendor";
#if !defined(EOS_BACKEND_VENDOR)
  d.compiled = false;
#endif
  d.supports_class = [](std::string_view kind) { return Supports(kind, {"base", "cam"}); };
  d.parse = [](const toml::table& tbl, const PartConfig& part, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    auto settings = std::make_shared<VendorSettings>();
    const auto* node = OneTable(tbl, "vendor", part.kind == "base", path, where);
    if (part.kind == "base")
      settings->base = ParseVendorBaseTable(*node, path, where + " [device.backend.vendor]");
    else if (node)
      Fail(path, where + ": [device.backend.vendor] is only implemented for class 'base'");
    return settings;
  };
  d.supports_primitive = [](const PrimitiveSpec& p) {
    return HasPrimitive(p, {"base.posture", "base.navigate", "base.gait", "face.register"});
  };
  d.create = [](std::string id, const PartConfig& part,
                const BackendSettings& settings) -> std::unique_ptr<hardware::Backend> {
#if defined(EOS_BACKEND_VENDOR)
    if (part.kind == "cam") return std::make_unique<vendor::VendorCamera>(std::move(id));
    const auto& a = *static_cast<const VendorSettings&>(settings).base;
    return std::make_unique<vendor::VendorBase>(
        std::move(id),
        vendor::VendorBaseConfig{.motion_host = a.motion_host,
                                 .motion_port = a.motion_port,
                                 .status_bind_host = a.status_bind_host,
                                 .status_port = a.status_port,
                                 .expected_sender = a.expected_sender,
                                 .robotserver_host = a.robotserver_host,
                                 .robotserver_port = a.robotserver_port,
                                 .io_timeout_ms = a.io_timeout_ms,
                                 .navigation_timeout_ms = a.navigation_timeout_ms,
                                 .transition_timeout_ms = a.transition_timeout_ms,
                                 .idle_stand_ms = a.idle_stand_ms,
                                 .map_id = a.map_id,
                                 .obstacle_host = a.obstacle_host,
                                 .obstacle_port = a.obstacle_port,
                                 .obstacle_timeout_ms = a.obstacle_timeout_ms,
                                 .obstacle_max_age_ms = a.obstacle_max_age_ms,
                                 .stream_axis_limit = a.stream_axis_limit,
                                 .stream_position_tolerance_m = a.stream_position_tolerance_m,
                                 .stream_yaw_tolerance_rad = a.stream_yaw_tolerance_rad,
                                 .stream_retarget_distance_m = a.stream_retarget_distance_m,
                                 .stream_retarget_yaw_rad = a.stream_retarget_yaw_rad});
#else
    (void)id;
    (void)part;
    (void)settings;
    return nullptr;
#endif
  };
  return d;
}
}  // namespace eos::backends

#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace eos::backends::config {
/// `[device.backend.vendor]` for an Island Base device. Network identity and
/// time budgets are deployment data and are never inherited from the vendor
/// library's convenience defaults.
struct VendorBaseDeviceConfig {
  std::string motion_host;
  std::uint16_t motion_port{0};
  std::string status_bind_host;
  std::uint16_t status_port{0};
  std::string expected_sender;
  std::string robotserver_host;
  std::uint16_t robotserver_port{0};
  std::uint64_t io_timeout_ms{0};
  std::uint64_t navigation_timeout_ms{0};
  std::uint64_t transition_timeout_ms{0};
  std::uint64_t idle_stand_ms{0};
  int map_id{0};
  std::optional<std::string> obstacle_host;
  std::uint16_t obstacle_port{0};
  std::uint64_t obstacle_timeout_ms{0};
  std::uint64_t obstacle_max_age_ms{0};
  int stream_axis_limit{12000};
  double stream_position_tolerance_m{0.05};
  double stream_yaw_tolerance_rad{0.05};
  double stream_retarget_distance_m{0.20};
  double stream_retarget_yaw_rad{0.30};
};

}  // namespace eos::backends::config

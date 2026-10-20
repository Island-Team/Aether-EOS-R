/// Strict standalone North HTTP configuration.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::ext::north_http {

inline constexpr int kMaximumRequestTimeoutMs = 630000;
/// Largest admitted connection or request-class capacity.
inline constexpr std::size_t kMaximumConnectionCapacity = 256;
/// Largest admitted audit segment before rotation.
inline constexpr std::uint64_t kMaximumAuditRotationSizeBytes = 1024ULL * 1024ULL * 1024ULL;
/// Largest admitted count of retained audit segments.
inline constexpr std::uint32_t kMaximumAuditRotationFiles = 64;
/// Largest admitted encoded audit retention across all segments.
inline constexpr std::uint64_t kMaximumAuditRetainedBytes = 256ULL * 1024ULL * 1024ULL;
/// Largest admitted delay between audit stream reconnect attempts.
inline constexpr int kMaximumAuditReconnectDelayMs = 60000;

/// Durable external ActionList audit settings.
struct ActionListAuditConfig {
  bool enabled{false};
  std::uint64_t rotation_size_bytes{16U * 1024U * 1024U};
  std::uint32_t rotation_files{8};
  int reconnect_delay_ms{1000};
};

/// Launch configuration for the HTTP-to-gRPC north HTTP process.
struct Config {
  std::string host{"127.0.0.1"};
  std::uint16_t port{16005};
  std::string upstream{"127.0.0.1:16003"};
  bool allow_remote{false};
  std::vector<std::string> allowed_origins{"https://os-island.com", "https://www.os-island.com",
                                           "http://localhost:3000", "http://127.0.0.1:3000"};
  std::size_t max_request_bytes{64U * 1024U * 1024U};
  std::size_t max_connections{64};
  std::size_t max_inflight_requests{32};
  std::size_t max_sse_streams{16};
  std::size_t max_monitor_streams{4};
  int connect_timeout_ms{2000};
  int request_timeout_ms{kMaximumRequestTimeoutMs};
  int sse_heartbeat_ms{15000};
  ActionListAuditConfig action_list_audit;
  eos::lib::LogConfig log;
};

/// Loads and validates one north HTTP TOML file. Unknown keys fail startup.
Config LoadConfig(const std::string& path);

/// True only for numeric and named loopback addresses accepted without remote opt-in.
bool IsLoopbackHost(const std::string& host);

/// Validates a gRPC target, including the absolute-path rule for Unix sockets.
void ValidateUpstream(const std::string& upstream);

}  // namespace eos::ext::north_http

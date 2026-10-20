/// Durable, non-authoritative ActionList audit projection for north HTTP.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace eos::ext::north_http {
class Client;

namespace audit {

/// Maximum serialized JSON-array bytes returned by one audit record page.
inline constexpr std::size_t kMaximumRecordPageBytes = 4U * 1024U * 1024U;
/// Maximum encoded bytes retained across all configured audit segments.
inline constexpr std::uint64_t kMaximumRetainedBytes = 256ULL * 1024ULL * 1024ULL;

/// Filesystem and retry limits for the external audit projection.
struct Config {
  /// Owner-only directory containing JSONL segments and cursor state.
  std::filesystem::path directory;
  /// Maximum segment bytes; a larger encoded record fails closed before append.
  std::uint64_t rotation_size_bytes = 16U * 1024U * 1024U;
  /// Number of newest append-only segments retained.
  std::size_t rotation_files = 8;
  /// Delay between failed snapshot or stream connection attempts.
  std::chrono::milliseconds reconnect_delay{1000};
};

/// Stable health state exposed by the audit diagnostics route.
enum class Health { kStopped, kHealthy, kDegraded };

/// Stable failure category; details are intentionally excluded from the public surface.
enum class ErrorCode {
  kNone,
  kInvalidConfig,
  kIo,
  kWriterConflict,
  kCorruptChain,
  kResourceExhausted
};

/// Thread-safe service status containing only operational metadata.
struct Status {
  /// Current persistence/consumer health.
  Health health = Health::kStopped;
  /// Stable local persistence failure category.
  ErrorCode error = ErrorCode::kNone;
  /// Whether the background consumer is expected to run.
  bool running = false;
  /// Opaque EOS ActionList boot epoch last durably projected.
  std::string epoch;
  /// EOS ActionList sequence last durably projected.
  std::uint64_t sequence = 0;
  std::uint64_t records = 0;  ///< Number of records in retained segments.
};

/// Bounded retained-record suffix prepared for the HTTP visibility route.
struct RecordPage {
  /// Selected records in oldest-first order.
  std::vector<nlohmann::json> records;
  /// True when older retained records were omitted by the count or byte bound.
  bool truncated = false;
  /// True when the newest record alone cannot fit within the byte bound.
  bool oversized = false;
  /// Serialized JSON-array bytes for `records`, including brackets and commas.
  std::size_t encoded_bytes = 2;
};

/// Persists and resumes a privacy-minimized projection of EOS ActionList events.
///
/// The injected client remains owned by process composition and must outlive this object.
/// Audit data is observational only and is never read by EOS core.
class Service final {
 public:
  /// Creates a stopped service; filesystem recovery occurs on Start().
  Service(Client& client, Config config);

  /// Stops and joins the worker before releasing persistence resources.
  ~Service();

  /// Services are process-owned and cannot be copied.
  Service(const Service&) = delete;

  /// Services are process-owned and cannot be copy-assigned.
  Service& operator=(const Service&) = delete;

  /// Recovers durable state and starts snapshot/stream consumption.
  /// Repeated calls are idempotent; invalid local state results in degraded status.
  void Start();

  /// Cancels any active stream and joins the worker. Safe to call repeatedly.
  void Stop() noexcept;

  /// Returns a synchronized operational snapshot without blocking on EOS.
  Status status() const;

  /// Returns a newest retained suffix bounded before records are copied.
  /// `maximum_encoded_bytes` measures the serialized JSON records array.
  RecordPage record_page(std::size_t limit,
                         std::size_t maximum_encoded_bytes = kMaximumRecordPageBytes) const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

/// Returns the stable lowercase wire name for a health state.
const char* ToString(Health value) noexcept;

/// Returns the stable lowercase wire name for an error category.
const char* ToString(ErrorCode value) noexcept;

}  // namespace audit
}  // namespace eos::ext::north_http

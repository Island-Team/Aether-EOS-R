/// Synchronous spdlog setup. Real-time loops must not call these sinks.
/// Replay tooling requires one JSON object per line.
#ifndef SRC_LIB_LOG_H_
#define SRC_LIB_LOG_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "spdlog/spdlog.h"

namespace eos::lib {

/// Unknown sink levels throw during configuration.
struct LogConfig {
  std::string console_level = "off";  // disabled stderr unless explicitly enabled
  std::string file_level = "info";    // human-readable routed files
  std::string jsonl_level = "trace";  // replay/tooling <service>/events.jsonl
  bool overwrite = false;             // retain the selected run instead of clearing it
  bool dedup = true;                  // suppress repeated records in file sinks
  double dedup_window_s = 30.0;       // positive repeat-suppression window
  bool save_attachments = false;      // persist binary evidence in the active log run
  std::uint64_t rotation_size_bytes = 64ULL * 1024ULL * 1024ULL;
  std::uint32_t rotation_files = 5;  // retained rotated copies, excluding the active file
};

/// Immutable identity included in every structured record from this process.
struct LogProcessContext {
  std::string service;
  std::string version;
  std::string launch_id;
  std::string run_id;
  std::uint64_t pid{0};
};

/// Resolves ISLAND_LOG, EOS_VAR, system mode, then ISLAND_PREFIX.
std::filesystem::path DefaultLogDirectory();

/// Returns a validated run directory; overwrite mode uses `current`.
std::filesystem::path ProcessLogDirectory(const std::filesystem::path& root,
                                          std::string_view service, bool overwrite = false);

void ConfigureLogging();

void ConfigureLogging(const LogConfig& config);

/// Idempotent. Overwrite mode removes managed files in the dedicated log directory.
void ConfigureLogging(const std::filesystem::path& log_dir, const LogConfig& config = {});

/// Service logging under a common root; JSONL includes process identity.
void ConfigureLogging(const std::filesystem::path& log_root, const std::string& service,
                      const LogConfig& config);

/// Sets the console threshold; file sinks retain defaults.
void ConfigureLogging(const std::filesystem::path& log_dir, const std::string& console_level);

/// Configures a dedicated directory named `attachments`. When `overwrite` is
/// true, that directory is cleared before use. An empty root disables
/// persistence.
void ConfigureLogAttachments(const std::filesystem::path& root, bool overwrite = false);

/// Before configuration, falls back to disabled stderr.
std::shared_ptr<spdlog::logger> GetLogger(const std::string& name);

/// Applies LogEvent's bounded, recursive redaction policy.
nlohmann::json SanitizeLogFields(const nlohmann::json& fields);

/// Bounds and removes control characters from a structured event name.
std::string SanitizeLogEvent(std::string event);

/// Emits a structured event. `fields` is sanitized before it reaches any
/// sink: sensitive values are redacted, control characters are removed, long
/// diagnostic strings are bounded, and endpoint query/userinfo data is
/// discarded. An optional, caller-controlled `summary` leads the human message;
/// otherwise the event name is made readable. The stable event and sanitized
/// fields remain available in JSONL. Summaries must describe safe metadata,
/// never request or media content.
void LogEvent(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level,
              const std::string& event, const nlohmann::json& fields = nlohmann::json::object(),
              std::string_view summary = {});

/// Persists unique `data` under the configured attachment root and emits an
/// event containing category, run-relative path, media type, and size metadata.
/// Duplicate content written by this process reuses the existing path.
/// Persistence failures are reported in metadata and never interrupt the
/// caller's media operation.
void LogEventWithAttachment(const std::shared_ptr<spdlog::logger>& logger,
                            spdlog::level::level_enum level, const std::string& event,
                            nlohmann::json fields, const std::string& category,
                            const std::string& extension, const std::string& media_type,
                            const std::vector<std::uint8_t>& data);

/// Emits once per process-wide logger/key pair.
void LogOnce(const std::shared_ptr<spdlog::logger>& logger, spdlog::level::level_enum level,
             const std::string& key, const std::string& event,
             const nlohmann::json& fields = nlohmann::json::object());

/// Explicitly adjusts the disabled-by-default stderr threshold at runtime.
/// The file sinks keep their configured thresholds.
void SetStderrLevel(const std::string& level);

/// RAII binding of a correlation ID for the calling thread: every event
/// emitted while the guard is alive carries the same correlation_id field.
/// Nested guards restore the outer ID on destruction.
class CorrelationScope {
 public:
  /// Binds `id`; mints a random UUID4-style ID when empty.
  explicit CorrelationScope(std::string id = "");
  ~CorrelationScope();
  CorrelationScope(const CorrelationScope&) = delete;
  CorrelationScope& operator=(const CorrelationScope&) = delete;

  /// The ID bound by this scope.
  const std::string& id() const { return id_; }

 private:
  std::string id_;
  std::string previous_;
};

/// Current thread's bound correlation ID, or empty when unbound.
/// Used by the JSONL sink; rarely needed by callers directly.
const std::string& CurrentCorrelationId();

/// Metadata/header name shared by HTTP and gRPC northbound transports.
inline constexpr char kRequestIdMetadataKey[] = "x-request-id";

/// Request IDs are bounded printable tokens safe for transport metadata and
/// structured logs: 1..96 ASCII letters, digits, '.', '_', ':', or '-'.
bool IsValidRequestId(std::string_view value);

/// Returns a validated incoming request ID, or mints a UUID-style ID when the
/// transport did not supply one. Invalid supplied values throw
/// std::invalid_argument so every transport can reject them consistently.
std::string ResolveRequestId(std::string_view value = {});

/// The configured process identity. Before configuration, fields are empty and
/// pid is zero.
const LogProcessContext& CurrentLogProcessContext();

}  // namespace eos::lib

#endif  // SRC_LIB_LOG_H_

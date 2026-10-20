#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "src/eos/lib/clock.h"
#include "src/eos/runtime/skills/core/provider.h"

namespace eos::runtime::skills {

enum class ExecutionStatus {
  kQueued,
  kRunning,
  kSucceeded,
  kRejected,
  kCancelled,
  kTimedOut,
  kFailed
};
const char* ExecutionStatusName(ExecutionStatus status);
bool IsTerminal(ExecutionStatus status);

struct ResolvedClaim {
  ClaimKind kind{ClaimKind::kResource};
  ClaimMode mode{ClaimMode::kExclusiveUse};
  std::string value;
};

struct ExecutionSnapshot {
  std::string execution_id;
  std::string parent_execution_id;
  std::string request_id;
  std::string skill_name;
  std::string skill_version;
  std::string runtime_id;
  std::string boot_id;
  EventTime event_time;
  ExecutionStatus status{ExecutionStatus::kQueued};
  std::uint64_t sequence{0};
  std::uint64_t latest_journal_sequence{0};
  bool cancellation_requested{false};
  std::string queue_reason;
  std::vector<ResolvedClaim> claims;
  std::vector<ResolvedClaim> active_device_claims;
  std::string reason_code;
  nlohmann::json result = nlohmann::json::object();
  std::vector<nlohmann::json> primitive_acks;
  std::string message;
  std::optional<std::chrono::system_clock::time_point> created_at;
  std::optional<std::chrono::system_clock::time_point> deadline_at;
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> cancellation_requested_at;
  std::optional<std::chrono::system_clock::time_point> finished_at;
  std::chrono::milliseconds queue_duration{0};
  std::chrono::milliseconds run_duration{0};
  std::chrono::milliseconds total_duration{0};
  struct ActivityView {
    std::string id;
    std::string name;
    std::chrono::system_clock::time_point started_at{};
    nlohmann::json fields = nlohmann::json::object();
  };
  std::optional<ActivityView> current_activity;
  struct Accounting {
    std::uint64_t primitive_calls{0};
    std::uint64_t resource_reads{0};
    std::uint64_t signals_received{0};
    std::uint64_t signals_emitted{0};
    std::uint64_t diagnostic_logs{0};
    std::uint64_t diagnostic_logs_dropped{0};
    std::uint64_t checkpoints{0};
    std::uint64_t checkpoints_dropped{0};
    std::uint64_t activity_events{0};
    std::uint64_t activity_events_dropped{0};
    std::chrono::milliseconds claim_wait_duration{0};
    std::chrono::milliseconds signal_wait_duration{0};
    std::chrono::milliseconds capability_duration{0};
  } accounting;
};

enum class JournalSource { kSystem, kSkill };
enum class JournalKind { kActivity, kCheckpoint, kSuppression };

struct JournalRecord {
  std::string event_id;
  JournalSource source{JournalSource::kSystem};
  JournalKind kind{JournalKind::kActivity};
  std::string name;
  std::string outcome;
  std::string message;
  std::optional<double> progress;
  nlohmann::json fields = nlohmann::json::object();
  std::chrono::system_clock::time_point timestamp{};
  std::chrono::milliseconds elapsed{0};
  std::optional<std::chrono::milliseconds> duration;
};

struct JournalEntry {
  enum class Kind { kLifecycle, kSignal, kRecord, kGap };
  std::uint64_t sequence{0};
  std::chrono::system_clock::time_point timestamp{};
  Kind kind{Kind::kLifecycle};
  ExecutionSnapshot execution;
  Signal signal;
  JournalRecord record;
  std::uint64_t gap_after_sequence{0};
  std::uint64_t gap_first_available_sequence{0};
};

struct ExecutionSelector {
  std::string skill_name;
  std::string skill_version;
  std::vector<ExecutionStatus> statuses;
};

struct MonitorUpdate {
  std::uint64_t sequence{0};
  bool reset{false};
  std::vector<ExecutionSnapshot> upserted;
  std::vector<std::string> removed_execution_ids;
};

struct TerminateManyResult {
  enum class Status { kAccepted, kExisting, kConflict };
  Status status{Status::kAccepted};
  std::string request_id;
  std::vector<std::string> execution_ids;
};

struct StartOptions {
  std::optional<std::chrono::milliseconds> timeout;
};
struct StartResult {
  enum class Status { kStarted, kExisting, kConflict };
  Status status{Status::kStarted};
  ExecutionSnapshot execution;
  operator std::string() const { return execution.execution_id; }
};

struct SignalReceipt {
  enum class Status { kAccepted, kExisting, kConflict };
  Status status{Status::kAccepted};
  std::string execution_id;
  std::string request_id;
  std::uint64_t sequence{0};
  std::string signal_id;
};

struct SkillManagerOptions {
  std::size_t worker_count{4};
  std::size_t max_execution_records{1024};
  std::size_t max_pending_signals{256};
  std::size_t max_journal_entries{1024};
  std::size_t max_monitor_updates{4096};
  std::size_t max_termination_requests{1024};
  std::size_t max_source_samples{256};
  std::size_t max_execution_state_bytes{64 * 1024};
  std::vector<SourceDescriptor> sources;
  const eos::lib::Clock* clock{nullptr};
  // Empty preserves the process-local journal without diagnostic persistence.
  std::optional<std::filesystem::path> journal_log_directory;
  bool journal_log_overwrite{false};
  std::uint64_t journal_log_rotation_size_bytes{64ULL * 1024ULL * 1024ULL};
  std::uint32_t journal_log_rotation_files{5};
};

}  // namespace eos::runtime::skills

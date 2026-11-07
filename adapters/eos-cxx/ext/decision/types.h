/// Backend-neutral value types used by an external EOS decision policy.
#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace eos::ext::decision {

using Json = nlohmann::json;

struct SkillReference {
  std::string name;
  std::string version;
};

enum class ClaimKind { kUnspecified, kDevice, kResource, kProviderContext };
enum class ClaimMode { kUnspecified, kSharedRead, kExclusiveUse };

struct SignalDescriptor {
  std::string name;
  Json payload_schema = Json::object();
};

struct ClaimTemplate {
  ClaimKind kind = ClaimKind::kUnspecified;
  ClaimMode mode = ClaimMode::kUnspecified;
  std::string argument_json_pointer;
};

struct PrimitiveReference {
  std::string name;
  std::string version;
};

struct SkillResource {
  std::string uri_template;
  std::string description;
};

struct ResolvedClaim {
  ClaimKind kind = ClaimKind::kUnspecified;
  ClaimMode mode = ClaimMode::kUnspecified;
  std::string identity;
};

struct SkillDescriptor {
  SkillReference reference;
  std::string description;
  Json parameters = Json::object();
  Json returns = Json::object();
  std::vector<PrimitiveReference> primitives;
  std::vector<SkillResource> resources;
  std::vector<SignalDescriptor> accepted_signals;
  std::vector<SignalDescriptor> emitted_signals;
  std::vector<ClaimTemplate> claims;
  bool reentrant = false;
};

enum class ExecutionStatus {
  kUnspecified,
  kQueued,
  kRunning,
  kSucceeded,
  kRejected,
  kCancelled,
  kTimedOut,
  kFailed,
};

struct ExecutionView {
  std::string execution_id;
  std::string runtime_id;
  std::string request_id;
  SkillReference reference;
  ExecutionStatus status = ExecutionStatus::kUnspecified;
  std::uint64_t sequence = 0;
  std::uint64_t latest_journal_sequence = 0;
  bool cancellation_requested = false;
  std::string queue_reason;
  std::vector<ResolvedClaim> claims;
  std::string reason_code;
  std::string message;
  Json result = Json::object();
  std::optional<std::chrono::system_clock::time_point> created_at;
  std::optional<std::chrono::system_clock::time_point> deadline_at;
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> cancellation_requested_at;
  std::optional<std::chrono::system_clock::time_point> finished_at;
  std::chrono::milliseconds queue_duration{0};
  std::chrono::milliseconds run_duration{0};
  std::chrono::milliseconds total_duration{0};
  struct CurrentActivity {
    std::string activity_id;
    std::string name;
    std::optional<std::chrono::system_clock::time_point> started_at;
    Json fields = Json::object();
  };
  struct Accounting {
    std::uint64_t primitive_calls = 0;
    std::uint64_t resource_reads = 0;
    std::uint64_t signals_received = 0;
    std::uint64_t signals_emitted = 0;
    std::uint64_t diagnostic_logs = 0;
    std::uint64_t diagnostic_logs_dropped = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t checkpoints_dropped = 0;
    std::uint64_t activity_events = 0;
    std::uint64_t activity_events_dropped = 0;
    std::chrono::milliseconds claim_wait_duration{0};
    std::chrono::milliseconds signal_wait_duration{0};
    std::chrono::milliseconds capability_duration{0};
  } accounting;
  std::optional<CurrentActivity> current_activity;
};

struct SignalView {
  enum class Direction { kUnspecified, kAgentToSkill, kSkillToAgent };

  std::string signal_id;
  std::string execution_id;
  Direction direction = Direction::kUnspecified;
  std::string name;
  Json payload = Json::object();
};

struct JournalRecordView {
  enum class Source { kUnspecified, kSystem, kSkill };
  enum class Kind { kUnspecified, kActivity, kCheckpoint, kSuppression };
  std::string event_id;
  std::string execution_id;
  Source source{Source::kUnspecified};
  Kind kind{Kind::kUnspecified};
  std::string name;
  std::string outcome;
  std::string message;
  std::chrono::system_clock::time_point timestamp{};
  std::chrono::milliseconds elapsed{0};
  std::optional<std::chrono::milliseconds> duration;
  std::optional<double> progress;
  Json fields = Json::object();
};

struct DecisionJournalEntry {
  std::string execution_id;
  std::uint64_t journal_sequence = 0;
  std::chrono::system_clock::time_point timestamp{};
  std::variant<std::monostate, ExecutionView, SignalView, JournalRecordView> value;
};

struct ExecutionSelector {
  std::optional<SkillReference> reference;
  std::vector<ExecutionStatus> statuses;
};

struct MonitorUpdateView {
  std::uint64_t monitor_sequence = 0;
  bool reset = false;
  std::vector<ExecutionView> upserted;
  std::vector<std::string> removed_execution_ids;
};

struct TerminateExecutionsAction {
  SkillReference reference;
  std::string request_id;
};

struct TerminateExecutionsReceipt {
  std::string request_id;
  std::vector<std::string> execution_ids;
};

struct SignalReceipt {
  std::string signal_id;
  std::string execution_id;
  std::string request_id;
  bool accepted = false;
};

struct StartAction {
  SkillReference reference;
  Json arguments = Json::object();
  std::string request_id;
  std::optional<std::chrono::milliseconds> timeout;
};

struct SignalAction {
  std::string execution_id;
  std::string request_id;
  std::string name;
  Json payload = Json::object();
};

struct TerminateAction {
  std::string execution_id;
};

using DecisionAction = std::variant<StartAction, SignalAction, TerminateAction>;

struct TransportError {
  int code = 0;
  std::string message;

  explicit operator bool() const { return code != 0; }
};

template <typename T>
struct Result {
  std::optional<T> value;
  TransportError error;

  bool ok() const { return value.has_value(); }
  explicit operator bool() const { return ok(); }

  static Result Success(T result) { return {std::move(result), {}}; }
  static Result Failure(int code, std::string message) {
    return {std::nullopt, {code, std::move(message)}};
  }
};

}  // namespace eos::ext::decision

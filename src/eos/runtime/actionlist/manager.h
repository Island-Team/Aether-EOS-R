#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "src/eos/runtime/skills/core/model.h"

namespace eos::runtime::actionlist {

/// A timestamped, ordered observation used by ActionList conditions.
struct StateFact {
  nlohmann::json value;
  std::string source;
  std::uint64_t sequence{0};
  std::chrono::system_clock::time_point event_time{};
  std::chrono::system_clock::time_point received_time{};
};

/// Point-in-time workflow state returned by the Workflow service.
struct RuntimeSnapshot {
  std::chrono::system_clock::time_point captured_at{};
  std::vector<skills::ExecutionSnapshot> executions;
  std::map<std::string, StateFact> facts;
  std::vector<nlohmann::json> action_lists;
  struct Cursor {
    std::string epoch;
    std::uint64_t sequence{0};
  } cursor;
};

/// Replay cursor identifying one boot epoch and event sequence.
using EventCursor = RuntimeSnapshot::Cursor;

/// Replayable ActionList or accepted-fact mutation.
struct Event {
  enum class Kind { kActionList, kFact };
  EventCursor cursor;
  std::chrono::system_clock::time_point observed_at{};
  Kind kind{Kind::kActionList};
  nlohmann::json action_list;
  std::string fact_key;
  StateFact fact;
};

/// Outcome of validating and reading an event cursor.
enum class ReplayStatus { kOk, kEpochMismatch, kExpired, kFuture };

/// Result returned by bounded replay/live-follow reads.
struct EventBatch {
  ReplayStatus status{ReplayStatus::kOk};
  std::vector<Event> events;
};

/// Narrow callbacks through which ActionList schedules and cancels skills.
struct SkillRunner {
  std::function<skills::StartResult(const std::string&, const std::string&, nlohmann::json,
                                    const std::string&)>
      start;
  std::function<bool(const std::string&)> cancel;
  std::function<std::vector<skills::ExecutionSnapshot>()> list_executions;
};

/// Narrow callbacks through which ActionList executes final primitives.
struct PrimitiveRunner {
  std::function<nlohmann::json(const std::string&, const std::string&, const std::string&,
                               nlohmann::json, const std::string&, const std::function<bool()>&)>
      execute;
  std::function<void(const std::string&)> cancel;
};

/// Latest resource snapshot available to resource-direct conditions.
struct ResourceSnapshot {
  nlohmann::json value;
  std::chrono::system_clock::time_point observed_at{};
};

/// Limits for the ActionList primitive worker pool.
struct ManagerOptions {
  std::size_t worker_count{4};
  std::size_t max_events{4096};
  /// Empty preserves process-local behavior; otherwise the manager owns this fixed-format store.
  std::filesystem::path journal_directory;
  /// Reads the latest EOS resource snapshot; enables resource-direct conditions.
  std::function<std::optional<ResourceSnapshot>(const std::string& uri)> read_resource;
  std::size_t max_workflows{1024};
  std::size_t max_workflow_bytes{1024 * 1024};
  std::size_t max_actions_per_workflow{1024};
  std::size_t journal_max_record_bytes{4U * 1024U * 1024U};
  std::uint64_t journal_max_bytes{256ULL * 1024ULL * 1024ULL};
};

/// Owns ActionList DAGs, state facts, and primitive action execution.
class Manager final {
 public:
  /// Constructs an ActionList manager with injected skill and primitive gates.
  explicit Manager(SkillRunner skill_runner, PrimitiveRunner primitive_runner = {},
                   ManagerOptions options = {});
  ~Manager();
  Manager(const Manager&) = delete;
  Manager& operator=(const Manager&) = delete;

  /// Validates, admits, and begins an ActionList definition.
  nlohmann::json Submit(const nlohmann::json& action_list);
  /// Returns one ActionList by workflow ID.
  std::optional<nlohmann::json> Get(const std::string& action_list_id) const;
  /// Returns all admitted ActionLists in workflow-ID order.
  std::vector<nlohmann::json> List() const;
  /// Returns a best-effort workflow/fact view plus current skill executions.
  RuntimeSnapshot Snapshot() const;
  /// Applies a newer fact and reevaluates eligible actions.
  bool UpdateFact(const std::string& key, StateFact fact);
  /// Observes a skill lifecycle transition emitted by SkillManager.
  void ObserveExecution(const skills::ExecutionSnapshot& execution);
  /// Observes a validated skill signal, including state.fact signals.
  void ObserveSignal(const skills::ExecutionSnapshot& execution, const skills::Signal& signal);
  /// Reconciles one ActionList against current skill execution snapshots.
  void Refresh(const std::string& action_list_id);
  /// Reevaluates actions gated by resource-direct conditions after an external
  /// state change such as a new detection publication.
  void NotifyExternalState();
  /// Replays events after a cursor, waiting briefly when already caught up.
  EventBatch WaitForEvents(const EventCursor& after, std::chrono::milliseconds wait) const;

 private:
  nlohmann::json SubmitImpl(const nlohmann::json& action_list);
  struct Record {
    nlohmann::json action_list;
    bool dispatch_deferred{false};
    std::map<std::string, std::uint64_t> action_tokens;
    std::map<std::string, std::chrono::system_clock::time_point>
        resource_condition_first_match;
  };
  struct SkillOwner {
    std::string workflow_id;
    std::string action_id;
    std::uint64_t token{0};
  };
  struct PendingSkillStart {
    SkillOwner owner;
    bool observed{false};
  };
  struct SkillStartJob {
    SkillOwner owner;
    std::string request_id;
    std::string skill_name;
    std::string skill_version;
    nlohmann::json arguments;
  };
  struct PrimitiveJob {
    nlohmann::json arguments;
    std::string correlation_id;
    std::string workflow_id;
    std::string action_id;
    std::string command_id;
    std::string primitive_name;
    std::string primitive_version;
    std::string device_id;
    std::shared_ptr<std::atomic_bool> cancel_requested;
  };
  struct PendingCancellation {
    bool primitive{false};
    std::string target_id;
  };

  bool ConditionMatches(const nlohmann::json& action_list, const nlohmann::json& action,
                        std::chrono::system_clock::time_point* matched_observed_at = nullptr) const;
  bool ResourceConditionMatches(
      const nlohmann::json& condition,
      std::chrono::system_clock::time_point* matched_observed_at = nullptr) const;
  void ObservePrimitive(const std::string& workflow_id, const std::string& action_id,
                        const std::string& status, nlohmann::json result = {},
                        const std::string& reason = {});
  void Advance(Record& action_list);
  void UpdateStatus(Record& action_list);
  void WorkerLoop();
  void RunPrimitive(PrimitiveJob job);
  void CancelPrimitive(const std::string& command_id);
  void DrainSkillStarts();
  void DrainDispatches();
  bool RequestStopLocked(nlohmann::json& action);
  void ApplyExecutionLocked(const SkillOwner& owner, const skills::ExecutionSnapshot& execution);
  bool TokenCurrentLocked(const SkillOwner& owner) const;
  void CancelSkillNoexcept(const std::string& execution_id) noexcept;
  void PublishActionListLocked(const nlohmann::json& action_list);
  void PublishFactLocked(const std::string& key, const StateFact& fact);
  void AppendJournalLocked(const std::string& kind, const nlohmann::json& payload);
  void CompactJournalLocked();
  void EnsureOperationalLocked() const;
  void EnterJournalFailStopLocked() noexcept;
  bool EvictTerminalWorkflowLocked();
  void RecoverJournal();

  SkillRunner skill_runner_;
  PrimitiveRunner primitive_runner_;
  mutable std::mutex mutex_;
  mutable std::condition_variable events_changed_;
  std::condition_variable work_available_;
  bool stopping_{false};
  std::uint64_t next_dispatch_token_{1};
  std::deque<SkillStartJob> pending_skill_starts_;
  std::map<std::string, PendingSkillStart> pending_skill_owners_;
  std::map<std::string, std::size_t> skill_cancellations_in_flight_;
  std::deque<PendingCancellation> pending_cancellations_;
  std::deque<PrimitiveJob> pending_;
  std::map<std::string, std::shared_ptr<std::atomic_bool>> primitive_cancellations_;
  std::vector<std::thread> workers_;
  std::map<std::string, Record> action_lists_;
  std::deque<std::string> workflow_order_;
  std::map<std::string, StateFact> facts_;
  std::map<std::string, std::map<std::string, std::uint64_t>> fact_source_sequences_;
  std::map<std::string, SkillOwner> execution_owners_;
  std::string event_epoch_;
  std::uint64_t next_event_sequence_{1};
  std::size_t max_events_{4096};
  std::size_t max_workflows_{1024};
  std::size_t max_workflow_bytes_{1024 * 1024};
  std::size_t max_actions_per_workflow_{1024};
  std::function<std::optional<ResourceSnapshot>(const std::string& uri)> read_resource_;
  std::deque<Event> events_;
  class JournalOwner;
  std::unique_ptr<JournalOwner> journal_;
  bool journal_failed_{false};
  bool journal_deferred_{false};
};

}  // namespace eos::runtime::actionlist

#pragma once
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "src/eos/runtime/skills/core/catalog.h"
#include "src/eos/runtime/skills/manager/record_store.h"
#include "src/eos/runtime/skills/manager/registry.h"
#include "src/eos/runtime/skills/manager/journald.h"
#include "src/eos/runtime/skills/manager/source_pool.h"
namespace eos::runtime::skills {

/// Optional non-owning observers for skill lifecycle and validated emitted signals.
struct SkillManagerObservers {
  std::function<void(const ExecutionSnapshot&)> execution;
  std::function<void(const ExecutionSnapshot&, const Signal&)> emitted_signal;
};

class SkillManager final {
 public:
  /// Supervises one provider as the compatibility `default` logical runtime.
  explicit SkillManager(Provider& provider, SkillManagerOptions options = {},
                        SkillManagerObservers observers = {});
  /// Supervises multiple logical runtimes with exact skill-owner routing.
  explicit SkillManager(std::vector<SkillRuntime> runtimes, SkillManagerOptions options = {},
                        SkillManagerObservers observers = {});
  ~SkillManager();
  SkillManager(const SkillManager&) = delete;
  SkillManager& operator=(const SkillManager&) = delete;
  std::vector<Metadata> ListSkills() const;
  const Metadata* GetSkill(const std::string&, const std::string&) const;
  StartResult Start(const std::string&, const std::string&, nlohmann::json, const std::string&,
                    StartOptions = {});
  StartResult SpawnChild(const std::string& parent_execution_id, const std::string& name,
                         const std::string& version, nlohmann::json arguments,
                         const std::string& request_id, StartOptions = {});
  bool SendMessage(const std::string& sender_execution_id,
                   const ProcessHandle& recipient, const std::string& channel,
                   nlohmann::json payload, const std::string& request_id,
                   std::chrono::milliseconds ttl);
  std::optional<IpcMessage> ReceiveMessage(const std::string& execution_id,
                                            const std::string& channel,
                                            std::chrono::milliseconds timeout,
                                            const std::string& correlation_id = {},
                                            const std::string& expected_sender_execution_id = {});
  std::optional<ProcessExit> AwaitExit(const std::string& requester_execution_id,
                                      const ProcessHandle&, std::chrono::milliseconds timeout);
  bool TerminateChild(const std::string& requester_execution_id, const ProcessHandle&);
  std::optional<ExecutionSnapshot> Get(const std::string&) const;
  std::optional<RetainedExecutionState> GetExecutionState(const std::string&) const;
  std::optional<RetainedExecutionState> WaitForExecutionState(
      const std::string&, std::uint64_t after_revision, std::chrono::milliseconds) const;
  std::vector<ExecutionSnapshot> ListExecutions() const;
  bool Terminate(const std::string&);
  TerminateManyResult TerminateMany(const std::string& skill_name,
                                    const std::string& skill_version,
                                    const std::string& request_id);
  SignalReceipt SignalExecution(const std::string&, const std::string& name, nlohmann::json payload,
                                const std::string& request_id);
  SignalReceipt Signal(const std::string& id, const std::string& name, nlohmann::json payload,
                       const std::string& request_id) {
    return SignalExecution(id, name, std::move(payload), request_id);
  }
  std::vector<ExecutionSnapshot> WaitForSnapshots(const std::string&, std::uint64_t,
                                                  std::chrono::milliseconds) const;
  std::vector<ExecutionSnapshot> WaitForEvents(const std::string& id, std::uint64_t after,
                                               std::chrono::milliseconds wait) const {
    return WaitForSnapshots(id, after, wait);
  }
  std::vector<JournalEntry> WaitForJournal(const std::string&, std::uint64_t,
                                           std::chrono::milliseconds) const;
  std::vector<MonitorUpdate> WaitForMonitor(const ExecutionSelector&, std::uint64_t,
                                             std::chrono::milliseconds) const;
  SourceSample PublishSource(const std::string&, std::int64_t, nlohmann::json);
  std::optional<SourceSample> QuerySource(const std::string&, SourceLookup,
                                          std::int64_t timestamp_ns = 0) const;
  std::optional<SourceSample> WaitForSource(const std::string&, std::uint64_t,
                                            std::chrono::milliseconds,
                                            Cancelled cancelled = {}) const;

 private:
  struct Job {
    std::shared_ptr<detail::Record> record;
    OwnedSkill owner;
    nlohmann::json arguments;
    std::string correlation_id;
    std::vector<ResolvedClaim> claims;
  };
  void WorkerLoop();
  void Run(const std::shared_ptr<detail::Record>&, const OwnedSkill&, nlohmann::json);
  void NotifyExecution(const ExecutionSnapshot&) const noexcept;
  void NotifySignal(const ExecutionSnapshot&, const eos::runtime::skills::Signal&) const noexcept;
  void PublishMonitor(const ExecutionSnapshot&);
  StartResult StartImpl(const std::string&, const std::string&, nlohmann::json,
                        const std::string&, StartOptions, const std::string& parent_execution_id);
  void FinalizeTerminalLocked(detail::Record&, ExecutionStatus, std::string);
  void CleanupTerminal(const std::shared_ptr<detail::Record>&, bool release_run);
  bool PublishState(const std::shared_ptr<detail::Record>&,
                    const std::optional<nlohmann::json>& schema, nlohmann::json payload,
                    bool related_process);
  std::optional<RetainedExecutionState> GetRelatedProcessState(
      const std::string& requester_execution_id, const ProcessHandle&) const;
  std::optional<RetainedExecutionState> WaitForRelatedProcessState(
      const std::string& requester_execution_id, const ProcessHandle&,
      std::uint64_t after_revision, std::chrono::milliseconds) const;
  static void ValidateProcessHandle(const ProcessHandle&, const detail::Record&);
  std::optional<std::size_t> RunnableJobLocked();
  std::vector<std::shared_ptr<detail::Record>> ExpireQueuedLocked();
  std::chrono::milliseconds TimeUntilNextDeadlineLocked() const;
  SkillManagerObservers observers_;
  SkillManagerOptions options_;
  const eos::lib::Clock& clock_;
  std::string boot_id_;
  RuntimeRegistry runtimes_;
  SkillJournald journald_;
  SourcePool sources_;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::condition_variable work_available_;
  std::condition_variable completions_finished_;
  bool stopping_{false};
  std::size_t active_runs_{0};
  detail::RecordStore store_;
  std::deque<Job> pending_;
  std::vector<std::pair<std::string, std::vector<ResolvedClaim>>> active_claims_;
  std::vector<std::thread> workers_;
  struct TerminationRequest {
    std::string skill_name;
    std::string skill_version;
    std::vector<std::string> execution_ids;
  };
  std::unordered_map<std::string, TerminationRequest> termination_requests_;
  std::deque<std::string> termination_request_order_;
  std::uint64_t next_process_generation_{1};
};
}  // namespace eos::runtime::skills

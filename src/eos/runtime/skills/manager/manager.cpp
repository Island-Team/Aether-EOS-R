#include "src/eos/runtime/skills/manager/manager.h"

#include <algorithm>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"
#include "src/eos/runtime/skills/manager/journald.h"
#include "src/eos/runtime/skills/manager/logging.h"
#include "src/eos/runtime/skills/manager/scheduler.h"

namespace eos::runtime::skills {
namespace {
constexpr std::size_t kMaxRequestIdBytes = 96;
constexpr std::size_t kMaxSignalPayloadBytes = 1024 * 1024;
constexpr std::chrono::milliseconds kMaximumTimeout = std::chrono::hours(1);
bool DeadlineExceeded(const detail::Record& record, eos::lib::MonotonicTime now) {
  return record.deadline && now >= *record.deadline;
}
ExecutionSnapshot SnapshotAt(const detail::Record& record, eos::lib::MonotonicTime now) {
  ExecutionSnapshot snapshot = record.snapshot;
  if (IsTerminal(snapshot.status)) return snapshot;
  snapshot.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - record.created_monotonic);
  if (record.started_monotonic) {
    snapshot.run_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - *record.started_monotonic);
  } else {
    snapshot.queue_duration = snapshot.total_duration;
  }
  return snapshot;
}
const eos::lib::Clock& ResolveClock(const SkillManagerOptions& options) {
  static const eos::lib::SystemClock clock;
  return options.clock == nullptr ? static_cast<const eos::lib::Clock&>(clock) : *options.clock;
}
const SignalDescriptor* FindSignal(const std::vector<SignalDescriptor>& signals,
                                   const std::string& name) {
  for (const auto& signal : signals)
    if (signal.name == name) return &signal;
  return nullptr;
}
const IpcChannelDescriptor* FindChannel(const Metadata& metadata, const std::string& name,
                                        bool sending) {
  for (const auto& channel : metadata.ipc_channels)
    if (channel.name == name && (sending ? channel.can_send : channel.can_receive)) return &channel;
  return nullptr;
}
void ValidateIpcPayload(const IpcChannelDescriptor& channel, const nlohmann::json& payload) {
  if (!payload.is_object()) throw std::invalid_argument("IPC payload must be an object");
  nlohmann::json_schema::json_validator validator(
      nullptr, nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(channel.schema);
  validator.validate(payload);
}
const char* ProcessStatus(ExecutionStatus status) {
  return ExecutionStatusName(status);
}
void PruneExpiredIpc(detail::Record& record, std::chrono::system_clock::time_point now) {
  record.mailbox.erase(
      std::remove_if(record.mailbox.begin(), record.mailbox.end(), [&](const IpcMessage& message) {
        return message.deadline && now >= *message.deadline;
      }),
      record.mailbox.end());
  record.ipc_requests.erase(
      std::remove_if(record.ipc_requests.begin(), record.ipc_requests.end(),
                     [&](const IpcMessage& message) {
                       return message.deadline && now >= *message.deadline;
                     }),
      record.ipc_requests.end());
}
}  // namespace

SkillManager::SkillManager(Provider& provider, SkillManagerOptions options,
                           SkillManagerObservers observers)
    : SkillManager(std::vector<SkillRuntime>{{"default", provider}}, options,
                   std::move(observers)) {}

SkillManager::SkillManager(std::vector<SkillRuntime> runtimes, SkillManagerOptions options,
                           SkillManagerObservers observers)
    : observers_(std::move(observers)),
      options_(options),
      clock_(ResolveClock(options_)),
      boot_id_(lib::MintUuidV7()),
      runtimes_(std::move(runtimes)),
      journald_(options_.max_monitor_updates, clock_, boot_id_,
                 options_.journal_log_directory,
                 options_.journal_log_overwrite, options_.journal_log_rotation_size_bytes,
                 options_.journal_log_rotation_files),
      sources_(options_.max_source_samples, clock_, boot_id_) {
  if (options_.worker_count == 0 || options_.max_execution_records == 0 ||
      options_.max_pending_signals == 0 || options_.max_journal_entries == 0 ||
      options_.max_termination_requests == 0 || options_.max_source_samples == 0 ||
      options_.max_execution_state_bytes == 0 ||
      options_.journal_log_rotation_size_bytes == 0 ||
      options_.journal_log_rotation_files == 0)
    throw std::invalid_argument("SkillManager limits must be greater than zero");
  for (const auto& skill : runtimes_.List())
    for (const auto& source : skill.sources) sources_.Declare(source);
  for (const auto& source : options_.sources) sources_.Declare(source);
  for (std::size_t i = 0; i < options_.worker_count; ++i)
    workers_.emplace_back(&SkillManager::WorkerLoop, this);
}

SkillManager::~SkillManager() {
  sources_.Close();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    for (const auto& [_, record] : store_.records) {
      std::lock_guard<std::mutex> record_lock(record->mutex);
      record->cancel_requested = true;
      record->snapshot.cancellation_requested = true;
      record->signal_available.notify_all();
    }
  }
  changed_.notify_all();
  work_available_.notify_all();
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
  std::unique_lock<std::mutex> lock(mutex_);
  completions_finished_.wait(lock, [this] { return active_runs_ == 0; });
}

SourceSample SkillManager::PublishSource(const std::string& topic, std::int64_t timestamp,
                                         nlohmann::json value) {
  return sources_.Publish(topic, timestamp, std::move(value));
}
std::optional<SourceSample> SkillManager::QuerySource(const std::string& topic,
                                                       SourceLookup lookup,
                                                       std::int64_t timestamp) const {
  return sources_.Query(topic, lookup, timestamp);
}
std::optional<SourceSample> SkillManager::WaitForSource(const std::string& topic,
                                                         std::uint64_t after,
                                                         std::chrono::milliseconds timeout,
                                                         Cancelled cancelled) const {
  return sources_.WaitForNewer(topic, after, timeout, cancelled);
}

std::vector<Metadata> SkillManager::ListSkills() const { return runtimes_.List(); }
const Metadata* SkillManager::GetSkill(const std::string& name, const std::string& version) const {
  const auto owner = runtimes_.Find(name, version);
  return owner ? owner->metadata : nullptr;
}

StartResult SkillManager::Start(const std::string& name, const std::string& version,
                                nlohmann::json arguments, const std::string& request_id,
                                StartOptions options) {
  return StartImpl(name, version, std::move(arguments), request_id, options, {});
}

StartResult SkillManager::SpawnChild(const std::string& parent_execution_id,
                                     const std::string& name, const std::string& version,
                                     nlohmann::json arguments, const std::string& request_id,
                                     StartOptions options) {
  if (parent_execution_id.empty()) throw std::invalid_argument("parent execution is required");
  return StartImpl(name, version, std::move(arguments), request_id, options,
                   parent_execution_id);
}

StartResult SkillManager::StartImpl(const std::string& name, const std::string& version,
                                    nlohmann::json arguments, const std::string& request_id,
                                    StartOptions options,
                                    const std::string& parent_execution_id) {
  if (name.empty() || version.empty()) throw std::invalid_argument("skill identity is required");
  if (request_id.empty() || request_id.size() > kMaxRequestIdBytes)
    throw std::invalid_argument("request_id must contain 1..96 bytes");
  if (options.timeout && (options.timeout->count() <= 0 || *options.timeout > kMaximumTimeout))
    throw std::invalid_argument("timeout must be greater than zero and at most one hour");
  const auto owner = runtimes_.Find(name, version);
  if (!owner) throw std::invalid_argument("skill was not admitted");
  owner->provider->ValidateArguments(name, version, arguments);
  const auto claims = ResolveClaims(*owner->metadata, arguments,
                                    owner->runtime_id + ":" + name + "@" + version);

  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto request = store_.requests.find(request_id); request != store_.requests.end()) {
    const auto found = store_.records.find(request->second);
    if (found != store_.records.end()) {
      std::lock_guard<std::mutex> record_lock(found->second->mutex);
      const bool identical = found->second->snapshot.skill_name == name &&
                             found->second->snapshot.skill_version == version &&
                             found->second->arguments == arguments &&
                             found->second->timeout == options.timeout &&
                             found->second->snapshot.parent_execution_id == parent_execution_id;
      return {identical ? StartResult::Status::kExisting : StartResult::Status::kConflict,
              found->second->snapshot};
    }
  }
  std::shared_ptr<detail::Record> parent;
  std::optional<ProcessHandle> parent_process;
  if (!parent_execution_id.empty()) {
    const auto found = store_.records.find(parent_execution_id);
    if (found == store_.records.end()) throw std::out_of_range("parent execution not found");
    parent = found->second;
    {
      std::lock_guard<std::mutex> parent_lock(parent->mutex);
      if (IsTerminal(parent->snapshot.status) || parent->cancel_requested ||
          DeadlineExceeded(*parent, clock_.MonotonicNow()))
        throw std::logic_error("parent execution is no longer active");
      if (parent->process_depth >= 8) throw std::length_error("maximum process depth exceeded");
      if (parent->children.size() >= 32)
        throw std::length_error("maximum live children exceeded");
      if (detail::ClaimsConflict(parent->snapshot.claims, claims))
        throw std::logic_error("parent and child claims would deadlock");
      parent_process = ProcessHandle{parent->snapshot.execution_id,
                                     parent->snapshot.parent_execution_id,
                                     parent->process_generation};
    }
    std::shared_ptr<detail::Record> ancestor = parent;
    while (ancestor) {
      std::string next;
      {
        std::lock_guard<std::mutex> ancestor_lock(ancestor->mutex);
        if (ancestor->snapshot.skill_name == name && ancestor->snapshot.skill_version == version)
          throw std::logic_error("cyclic skill spawn is not allowed");
        next = ancestor->snapshot.parent_execution_id;
      }
      if (next.empty()) break;
      const auto next_record = store_.records.find(next);
      ancestor = next_record == store_.records.end() ? nullptr : next_record->second;
    }
  }
  if (stopping_) throw std::runtime_error("skill manager is stopping");
  for (const auto& evicted : store_.EvictForCapacity(options_.max_execution_records))
    journald_.Remove(evicted);
  if (store_.records.size() >= options_.max_execution_records)
    throw std::runtime_error("skill execution record limit reached");
  auto record = std::make_shared<detail::Record>();
  record->snapshot.execution_id = lib::MintUuidV7();
  record->snapshot.parent_execution_id = parent_execution_id;
  record->snapshot.request_id = request_id;
  record->snapshot.skill_name = name;
  record->snapshot.skill_version = version;
  record->snapshot.runtime_id = owner->runtime_id;
  record->snapshot.boot_id = boot_id_;
  record->snapshot.claims = claims;
  record->arguments = arguments;
  record->timeout = options.timeout;
  record->process_generation = next_process_generation_++;
  if (next_process_generation_ == 0) ++next_process_generation_;
  record->parent_process = std::move(parent_process);
  record->created_monotonic = clock_.MonotonicNow();
  if (options.timeout) record->deadline = record->created_monotonic + *options.timeout;
  record->process_depth = parent ? parent->process_depth + 1 : 0;
  record->snapshot.created_at = clock_.WallNow();
  if (options.timeout) record->snapshot.deadline_at = *record->snapshot.created_at + *options.timeout;
  for (const auto& active : active_claims_)
    if (detail::ClaimsConflict(claims, active.second))
      record->snapshot.queue_reason = "claim.conflict";
  journald_.AppendLifecycle(*record, options_.max_journal_entries);
  store_.records.emplace(record->snapshot.execution_id, record);
  store_.requests.emplace(request_id, record->snapshot.execution_id);
  store_.order.push_back(record->snapshot.execution_id);
  if (parent) {
    std::lock_guard<std::mutex> parent_lock(parent->mutex);
    parent->children.insert(record->snapshot.execution_id);
  }
  journald_.Publish(record->snapshot);
  Job job;
  job.record = record;
  job.owner = *owner;
  job.arguments = std::move(arguments);
  job.correlation_id = eos::lib::CurrentCorrelationId();
  job.claims = claims;
  pending_.push_back(std::move(job));
  LogSkillEvent(spdlog::level::debug, "skill_execution_routed",
                {{"execution_id", record->snapshot.execution_id},
                 {"skill", name},
                 {"version", version},
                 {"runtime_id", owner->runtime_id}});
  LogExecutionEvent(spdlog::level::debug, "skill_execution_queued", record->snapshot.execution_id,
                    name, version, request_id, "queued", "", record->snapshot.sequence);
  changed_.notify_all();
  work_available_.notify_all();
  return {StartResult::Status::kStarted, record->snapshot};
}

std::optional<ExecutionSnapshot> SkillManager::Get(const std::string& id) const {
  std::shared_ptr<detail::Record> record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.records.find(id);
    if (it == store_.records.end()) return std::nullopt;
    record = it->second;
  }
  std::lock_guard<std::mutex> lock(record->mutex);
  return SnapshotAt(*record, clock_.MonotonicNow());
}

std::optional<RetainedExecutionState> SkillManager::GetExecutionState(
    const std::string& id) const {
  std::shared_ptr<detail::Record> record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = store_.records.find(id);
    if (found == store_.records.end()) return std::nullopt;
    record = found->second;
  }
  std::lock_guard<std::mutex> lock(record->mutex);
  return record->execution_state;
}

std::optional<RetainedExecutionState> SkillManager::WaitForExecutionState(
    const std::string& id, std::uint64_t after_revision,
    std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> service_lock(mutex_);
  const auto found = store_.records.find(id);
  if (found == store_.records.end()) return std::nullopt;
  const auto record = found->second;
  changed_.wait_for(service_lock, timeout, [&] {
    std::lock_guard<std::mutex> record_lock(record->mutex);
    return stopping_ || IsTerminal(record->snapshot.status) ||
           (record->execution_state && record->execution_state->revision > after_revision);
  });
  service_lock.unlock();
  std::lock_guard<std::mutex> record_lock(record->mutex);
  if (!record->execution_state || record->execution_state->revision <= after_revision)
    return std::nullopt;
  return record->execution_state;
}

std::optional<RetainedExecutionState> SkillManager::GetRelatedProcessState(
    const std::string& requester_execution_id, const ProcessHandle& process) const {
  std::lock_guard<std::mutex> service_lock(mutex_);
  const auto requester_it = store_.records.find(requester_execution_id);
  const auto target_it = store_.records.find(process.execution_id);
  if (requester_it == store_.records.end() || target_it == store_.records.end())
    throw std::out_of_range("related process not found");
  if (requester_it == target_it)
    throw std::invalid_argument("state read requires a related process handle");
  const auto& requester = requester_it->second;
  const auto& target = target_it->second;
  std::scoped_lock records_lock(requester->mutex, target->mutex);
  ValidateProcessHandle(process, *target);
  const bool related = target->snapshot.parent_execution_id == requester_execution_id ||
                       requester->snapshot.parent_execution_id == process.execution_id;
  if (!related) throw std::invalid_argument("state read is restricted to direct related processes");
  return target->related_process_state;
}

std::optional<RetainedExecutionState> SkillManager::WaitForRelatedProcessState(
    const std::string& requester_execution_id, const ProcessHandle& process,
    std::uint64_t after_revision, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> service_lock(mutex_);
  const auto requester_it = store_.records.find(requester_execution_id);
  const auto target_it = store_.records.find(process.execution_id);
  if (requester_it == store_.records.end() || target_it == store_.records.end())
    throw std::out_of_range("related process not found");
  if (requester_it == target_it)
    throw std::invalid_argument("state read requires a related process handle");
  const auto requester = requester_it->second;
  const auto target = target_it->second;
  {
    std::scoped_lock records_lock(requester->mutex, target->mutex);
    ValidateProcessHandle(process, *target);
    const bool related = target->snapshot.parent_execution_id == requester_execution_id ||
                         requester->snapshot.parent_execution_id == process.execution_id;
    if (!related)
      throw std::invalid_argument("state read is restricted to direct related processes");
  }
  changed_.wait_for(service_lock, timeout, [&] {
    std::lock_guard<std::mutex> target_lock(target->mutex);
    return stopping_ || IsTerminal(target->snapshot.status) ||
           (target->related_process_state &&
            target->related_process_state->revision > after_revision);
  });
  service_lock.unlock();
  std::lock_guard<std::mutex> target_lock(target->mutex);
  if (!target->related_process_state ||
      target->related_process_state->revision <= after_revision)
    return std::nullopt;
  return target->related_process_state;
}

bool SkillManager::PublishState(const std::shared_ptr<detail::Record>& record,
                                const std::optional<nlohmann::json>& schema,
                                nlohmann::json payload, bool related_process) {
  if (!schema || !payload.is_object() || payload.dump().size() > options_.max_execution_state_bytes)
    return false;
  try {
    nlohmann::json_schema::json_validator validator(
        nullptr, nlohmann::json_schema::default_string_format_check);
    validator.set_root_schema(*schema);
    validator.validate(payload);
  } catch (...) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (IsTerminal(record->snapshot.status)) return false;
    auto& retained = related_process ? record->related_process_state : record->execution_state;
    const std::uint64_t revision = retained ? retained->revision + 1 : 1;
    retained = RetainedExecutionState{revision, clock_.WallNow(), std::move(payload)};
  }
  changed_.notify_all();
  return true;
}

std::vector<ExecutionSnapshot> SkillManager::ListExecutions() const {
  std::vector<ExecutionSnapshot> result;
  std::lock_guard<std::mutex> lock(mutex_);
  result.reserve(store_.order.size());
  const auto now = clock_.MonotonicNow();
  for (const auto& id : store_.order)
    if (auto found = store_.records.find(id); found != store_.records.end()) {
      std::lock_guard<std::mutex> record_lock(found->second->mutex);
      result.push_back(SnapshotAt(*found->second, now));
    }
  return result;
}

bool SkillManager::Terminate(const std::string& id) {
  std::optional<ExecutionSnapshot> observed;
  std::optional<ExecutionSnapshot> cancellation_requested;
  std::shared_ptr<detail::Record> terminal_record;
  std::vector<std::string> children;
  {
    std::lock_guard<std::mutex> service_lock(mutex_);
    auto found = store_.records.find(id);
    if (found == store_.records.end()) return false;
    const auto& record = found->second;
    std::lock_guard<std::mutex> lock(record->mutex);
    if (IsTerminal(record->snapshot.status)) return true;
    children.assign(record->children.begin(), record->children.end());
    if (!record->cancel_requested) {
      record->cancel_requested = true;
      record->snapshot.cancellation_requested = true;
      record->snapshot.cancellation_requested_at = clock_.WallNow();
      journald_.AppendLifecycle(*record, options_.max_journal_entries);
      cancellation_requested = record->snapshot;
      journald_.Publish(record->snapshot);
      if (record->snapshot.status == ExecutionStatus::kQueued) {
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [&](const Job& job) { return job.record == record; }),
                       pending_.end());
        FinalizeTerminalLocked(*record, ExecutionStatus::kCancelled, "cancelled");
        observed = record->snapshot;
        terminal_record = record;
      }
      record->signal_available.notify_all();
      changed_.notify_all();
      work_available_.notify_all();
    }
  }
  if (terminal_record) CleanupTerminal(terminal_record, false);
  for (const auto& child : children) (void)Terminate(child);
  if (cancellation_requested) {
    const auto& snapshot = *cancellation_requested;
    LogExecutionEvent(spdlog::level::debug, "skill_cancellation_requested", snapshot.execution_id,
                      snapshot.skill_name, snapshot.skill_version, snapshot.request_id,
                      ExecutionStatusName(snapshot.status), "", snapshot.sequence);
  }
  if (observed) NotifyExecution(*observed);
  return true;
}

void SkillManager::FinalizeTerminalLocked(detail::Record& record, ExecutionStatus status,
                                          std::string reason) {
  const auto now_mono = clock_.MonotonicNow();
  record.finished_monotonic = now_mono;
  record.snapshot.finished_at = clock_.WallNow();
  record.snapshot.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      now_mono - record.created_monotonic);
  if (record.started_monotonic) {
    record.snapshot.run_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        now_mono - *record.started_monotonic);
  } else {
    record.snapshot.queue_duration = record.snapshot.total_duration;
  }
  record.snapshot.status = status;
  record.snapshot.reason_code = std::move(reason);
  record.snapshot.queue_reason.clear();
  record.snapshot.current_activity.reset();
  record.active_activities.clear();
  record.activity_order.clear();
  journald_.AppendLifecycle(record, options_.max_journal_entries);
  journald_.Publish(record.snapshot);
}

void SkillManager::CleanupTerminal(const std::shared_ptr<detail::Record>& record,
                                   bool release_run) {
  std::string execution_id;
  std::string parent_id;
  std::vector<std::string> children;
  {
    std::lock_guard<std::mutex> lock(record->mutex);
    execution_id = record->snapshot.execution_id;
    parent_id = record->snapshot.parent_execution_id;
    children.assign(record->children.begin(), record->children.end());
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!parent_id.empty()) {
      const auto parent = store_.records.find(parent_id);
      if (parent != store_.records.end()) {
        std::lock_guard<std::mutex> parent_lock(parent->second->mutex);
        parent->second->children.erase(execution_id);
      }
    }
    if (release_run) {
      active_claims_.erase(std::remove_if(active_claims_.begin(), active_claims_.end(),
                                          [&](const auto& active) {
                                            return active.first == execution_id;
                                          }),
                           active_claims_.end());
      --active_runs_;
    }
  }
  for (const auto& child : children) (void)Terminate(child);
  completions_finished_.notify_all();
  work_available_.notify_all();
}

TerminateManyResult SkillManager::TerminateMany(const std::string& skill_name,
                                                const std::string& skill_version,
                                                const std::string& request_id) {
  if (skill_name.empty()) throw std::invalid_argument("skill name is required");
  if (request_id.empty() || request_id.size() > kMaxRequestIdBytes)
    throw std::invalid_argument("request_id must contain 1..96 bytes");
  std::vector<std::string> targets;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto old = termination_requests_.find(request_id);
        old != termination_requests_.end()) {
      const bool same = old->second.skill_name == skill_name &&
                        old->second.skill_version == skill_version;
      return {same ? TerminateManyResult::Status::kExisting
                   : TerminateManyResult::Status::kConflict,
              request_id, old->second.execution_ids};
    }
    for (const auto& id : store_.order) {
      const auto found = store_.records.find(id);
      if (found == store_.records.end()) continue;
      std::lock_guard<std::mutex> record_lock(found->second->mutex);
      const auto& snapshot = found->second->snapshot;
      if (!IsTerminal(snapshot.status) && snapshot.skill_name == skill_name &&
          (skill_version.empty() || snapshot.skill_version == skill_version))
        targets.push_back(id);
    }
    std::sort(targets.begin(), targets.end());
    termination_requests_.emplace(
        request_id, TerminationRequest{skill_name, skill_version, targets});
    termination_request_order_.push_back(request_id);
    while (termination_request_order_.size() > options_.max_termination_requests) {
      termination_requests_.erase(termination_request_order_.front());
      termination_request_order_.pop_front();
    }
  }
  for (const auto& id : targets) (void)Terminate(id);
  return {TerminateManyResult::Status::kAccepted, request_id, std::move(targets)};
}

SignalReceipt SkillManager::SignalExecution(const std::string& id, const std::string& name,
                                            nlohmann::json payload, const std::string& request_id) {
  if (request_id.empty() || request_id.size() > kMaxRequestIdBytes)
    throw std::invalid_argument("request_id must contain 1..96 bytes");
  if (name == "CANCEL") throw std::invalid_argument("CANCEL is manager-reserved");
  if (payload.dump().size() > kMaxSignalPayloadBytes)
    throw std::length_error("SIGNAL payload too large");
  std::shared_ptr<detail::Record> record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.records.find(id);
    if (it == store_.records.end()) throw std::out_of_range("execution not found");
    record = it->second;
  }
  {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (auto old = record->signal_requests.find(request_id); old != record->signal_requests.end()) {
      if (old->second.signal.name == name && old->second.signal.payload == payload) {
        auto receipt = old->second.receipt;
        receipt.status = SignalReceipt::Status::kExisting;
        return receipt;
      }
      return {SignalReceipt::Status::kConflict, id, request_id, old->second.receipt.sequence,
              old->second.receipt.signal_id};
    }
    if (IsTerminal(record->snapshot.status)) throw std::logic_error("execution is terminal");
  }
  const auto owner = runtimes_.Find(record->snapshot.skill_name, record->snapshot.skill_version);
  if (!owner || !owner->metadata->supports_signals)
    throw std::runtime_error("provider does not support SIGNALs");
  const auto* declaration = FindSignal(owner->metadata->accepted_signals, name);
  if (!declaration) throw std::invalid_argument("SIGNAL is not accepted by this skill");
  runtimes_.ValidateSignal(*owner, *declaration, payload);
  std::lock_guard<std::mutex> lock(record->mutex);
  if (auto old = record->signal_requests.find(request_id); old != record->signal_requests.end()) {
    if (old->second.signal.name == name && old->second.signal.payload == payload) {
      auto receipt = old->second.receipt;
      receipt.status = SignalReceipt::Status::kExisting;
      return receipt;
    }
    return {SignalReceipt::Status::kConflict, id, request_id, old->second.receipt.sequence,
            old->second.receipt.signal_id};
  }
  if (IsTerminal(record->snapshot.status)) throw std::logic_error("execution is terminal");
  if (record->inbound.size() >= options_.max_pending_signals)
    throw std::length_error("pending SIGNAL queue is full");
  SignalReceipt receipt{SignalReceipt::Status::kAccepted, id, request_id,
                        record->snapshot.latest_journal_sequence, lib::MintUuidV7()};
  detail::SignalEnvelope envelope{
      {name, std::move(payload), receipt.signal_id, id}, request_id, receipt};
  record->signal_requests.emplace(request_id, envelope);
  record->inbound.push_back(std::move(envelope));
  LogSkillEvent(spdlog::level::debug, "skill_signal_accepted",
                {{"execution_id", id},
                 {"signal", name},
                 {"signal_id", receipt.signal_id},
                 {"pending_signals", record->inbound.size()}});
  record->signal_available.notify_one();
  return receipt;
}

bool SkillManager::SendMessage(const std::string& sender_execution_id,
                               const ProcessHandle& recipient, const std::string& channel,
                               nlohmann::json payload, const std::string& request_id,
                               std::chrono::milliseconds ttl) {
  if (sender_execution_id.empty() || recipient.execution_id.empty())
    throw std::invalid_argument("IPC sender and recipient are required");
  if (request_id.empty() || request_id.size() > kMaxRequestIdBytes)
    throw std::invalid_argument("request_id must contain 1..96 bytes");
  if (ttl.count() <= 0 || ttl > kMaximumTimeout)
    throw std::invalid_argument("IPC TTL must be greater than zero and at most one hour");
  if (payload.dump().size() > kMaxSignalPayloadBytes) throw std::length_error("IPC payload too large");
  std::shared_ptr<detail::Record> sender;
  std::shared_ptr<detail::Record> target;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto sender_it = store_.records.find(sender_execution_id);
    const auto target_it = store_.records.find(recipient.execution_id);
    if (sender_it == store_.records.end() || target_it == store_.records.end())
      throw std::out_of_range("IPC process not found");
    sender = sender_it->second;
    target = target_it->second;
  }
  ExecutionSnapshot sender_snapshot;
  ExecutionSnapshot target_snapshot;
  {
    std::lock_guard<std::mutex> lock(sender->mutex);
    sender_snapshot = sender->snapshot;
  }
  {
    std::lock_guard<std::mutex> lock(target->mutex);
    ValidateProcessHandle(recipient, *target);
    target_snapshot = target->snapshot;
  }
  const bool related = target_snapshot.parent_execution_id == sender_execution_id ||
                       sender_snapshot.parent_execution_id == recipient.execution_id;
  if (!related)
    throw std::invalid_argument("IPC is restricted to parent-child processes");
  const auto sender_owner = runtimes_.Find(sender_snapshot.skill_name, sender_snapshot.skill_version);
  const auto target_owner = runtimes_.Find(target_snapshot.skill_name, target_snapshot.skill_version);
  if (!sender_owner || !target_owner) throw std::runtime_error("IPC process owner is unavailable");
  const auto* outgoing = FindChannel(*sender_owner->metadata, channel, true);
  const auto* incoming = FindChannel(*target_owner->metadata, channel, false);
  if (!outgoing || !incoming) throw std::invalid_argument("IPC channel is not declared");
  ValidateIpcPayload(*outgoing, payload);
  ValidateIpcPayload(*incoming, payload);

  std::unique_lock<std::mutex> lock(target->mutex);
  if (IsTerminal(target->snapshot.status) || target->cancel_requested ||
      DeadlineExceeded(*target, clock_.MonotonicNow()))
    throw std::logic_error("IPC recipient is no longer active");
  const auto prune_expired = [&] { PruneExpiredIpc(*target, clock_.WallNow()); };
  prune_expired();
  const auto prior_request = [&] {
    return std::find_if(target->ipc_requests.begin(), target->ipc_requests.end(),
                        [&](const IpcMessage& message) {
                          return message.sender_execution_id == sender_execution_id &&
                                 message.request_id == request_id;
                        });
  };
  if (const auto old = prior_request(); old != target->ipc_requests.end()) {
    if (old->channel == channel && old->payload == payload)
      return false;
    throw std::logic_error("duplicate IPC request conflict");
  }
  if (target->mailbox.size() >= options_.max_pending_signals) {
    target->signal_available.wait_for(lock, ttl, [&] {
      prune_expired();
      return IsTerminal(target->snapshot.status) || target->cancel_requested ||
              DeadlineExceeded(*target, clock_.MonotonicNow()) ||
              target->mailbox.size() < options_.max_pending_signals;
    });
    prune_expired();
    if (target->mailbox.size() >= options_.max_pending_signals)
      throw std::length_error("IPC mailbox is full");
  }
  if (IsTerminal(target->snapshot.status) || target->cancel_requested ||
      DeadlineExceeded(*target, clock_.MonotonicNow()))
    throw std::logic_error("IPC recipient is no longer active");
  lock.unlock();
  {
    std::lock_guard<std::mutex> sender_lock(sender->mutex);
    if (IsTerminal(sender->snapshot.status) || sender->cancel_requested ||
        DeadlineExceeded(*sender, clock_.MonotonicNow()))
      throw std::logic_error("IPC sender is no longer active");
  }
  lock.lock();
  prune_expired();
  if (IsTerminal(target->snapshot.status) || target->cancel_requested ||
      DeadlineExceeded(*target, clock_.MonotonicNow()))
    throw std::logic_error("IPC recipient is no longer active");
  if (target->mailbox.size() >= options_.max_pending_signals)
    throw std::length_error("IPC mailbox is full");
  if (const auto old = prior_request(); old != target->ipc_requests.end()) {
    if (old->channel == channel && old->payload == payload)
      return false;
    throw std::logic_error("duplicate IPC request conflict");
  }
  IpcMessage message;
  message.message_id = lib::MintUuidV7();
  message.request_id = request_id;
  message.correlation_id = request_id;
  message.sender_execution_id = sender_execution_id;
  message.recipient_execution_id = recipient.execution_id;
  message.channel = channel;
  message.payload = std::move(payload);
  message.event_time.observed_at = clock_.WallNow();
  message.event_time.observed_monotonic_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.MonotonicNow().time_since_epoch())
          .count();
  message.event_time.boot_id = boot_id_;
  message.event_time.sequence = target->mailbox.empty()
                                    ? 1
                                    : target->mailbox.back().event_time.sequence + 1;
  message.event_time.event_id = message.message_id;
  message.deadline = message.event_time.observed_at + ttl;
  while (target->ipc_requests.size() >= options_.max_pending_signals) {
    target->ipc_requests.pop_front();
  }
  target->ipc_requests.push_back(message);
  target->mailbox.push_back(std::move(message));
  target->signal_available.notify_all();
  return true;
}

std::optional<IpcMessage> SkillManager::ReceiveMessage(const std::string& execution_id,
                                                        const std::string& channel,
                                                        std::chrono::milliseconds timeout,
                                                        const std::string& correlation_id,
                                                        const std::string& expected_sender_execution_id) {
  std::shared_ptr<detail::Record> record;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = store_.records.find(execution_id);
    if (found == store_.records.end()) throw std::out_of_range("IPC process not found");
    record = found->second;
  }
  std::unique_lock<std::mutex> lock(record->mutex);
  const auto prune_expired = [&] { PruneExpiredIpc(*record, clock_.WallNow()); };
  auto available = [&] {
    prune_expired();
    return record->cancel_requested || IsTerminal(record->snapshot.status) ||
           std::any_of(record->mailbox.begin(), record->mailbox.end(), [&](const auto& message) {
              return (channel.empty() || message.channel == channel) &&
                     (correlation_id.empty() || message.correlation_id == correlation_id) &&
                     (expected_sender_execution_id.empty() ||
                      message.sender_execution_id == expected_sender_execution_id);
           });
  };
  record->signal_available.wait_for(lock, timeout, available);
  prune_expired();
  const auto found = std::find_if(record->mailbox.begin(), record->mailbox.end(),
                                  [&](const auto& message) {
                                     return (channel.empty() || message.channel == channel) &&
                                            (correlation_id.empty() ||
                                             message.correlation_id == correlation_id) &&
                                            (expected_sender_execution_id.empty() ||
                                             message.sender_execution_id ==
                                                 expected_sender_execution_id);
                                  });
  if (found == record->mailbox.end()) return std::nullopt;
  IpcMessage result = std::move(*found);
  record->mailbox.erase(found);
  record->signal_available.notify_all();
  return result;
}

std::optional<ProcessExit> SkillManager::AwaitExit(const std::string& requester_execution_id,
                                                   const ProcessHandle& process,
                                                   std::chrono::milliseconds timeout) {
  std::shared_ptr<detail::Record> record;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto found = store_.records.find(process.execution_id);
    if (found == store_.records.end()) throw std::out_of_range("child process not found");
    record = found->second;
    {
      std::lock_guard<std::mutex> record_lock(record->mutex);
      ValidateProcessHandle(process, *record);
      if (record->snapshot.parent_execution_id != requester_execution_id)
        throw std::invalid_argument("process is not a child of requester");
    }
    changed_.wait_for(lock, timeout, [&] {
      std::lock_guard<std::mutex> record_lock(record->mutex);
      return stopping_ || IsTerminal(record->snapshot.status);
    });
  }
  std::lock_guard<std::mutex> lock(record->mutex);
  if (!IsTerminal(record->snapshot.status)) return std::nullopt;
  return ProcessExit{ProcessStatus(record->snapshot.status), record->snapshot.reason_code,
                     record->snapshot.result, record->snapshot.message};
}

bool SkillManager::TerminateChild(const std::string& requester_execution_id,
                                   const ProcessHandle& process) {
  {
    std::lock_guard<std::mutex> service_lock(mutex_);
    const auto found = store_.records.find(process.execution_id);
    if (found == store_.records.end()) return false;
    std::lock_guard<std::mutex> record_lock(found->second->mutex);
    ValidateProcessHandle(process, *found->second);
    if (found->second->snapshot.parent_execution_id != requester_execution_id)
      throw std::invalid_argument("process is not a child of requester");
  }
  return Terminate(process.execution_id);
}

void SkillManager::ValidateProcessHandle(const ProcessHandle& process,
                                         const detail::Record& record) {
  if (process.generation == 0 || process.generation != record.process_generation ||
      process.execution_id != record.snapshot.execution_id ||
      process.parent_execution_id != record.snapshot.parent_execution_id)
    throw std::invalid_argument("invalid process handle");
}

std::vector<ExecutionSnapshot> SkillManager::WaitForSnapshots(
    const std::string& id, std::uint64_t after, std::chrono::milliseconds wait) const {
  std::unique_lock<std::mutex> service_lock(mutex_);
  auto found = store_.records.find(id);
  if (found == store_.records.end()) return {};
  auto record = found->second;
  changed_.wait_for(service_lock, wait, [&] {
    std::lock_guard<std::mutex> lock(record->mutex);
    return stopping_ || record->snapshot.sequence > after;
  });
  service_lock.unlock();
  std::vector<ExecutionSnapshot> result;
  std::lock_guard<std::mutex> lock(record->mutex);
  for (const auto& snapshot : record->lifecycle)
    if (snapshot.sequence > after) result.push_back(snapshot);
  return result;
}

std::vector<JournalEntry> SkillManager::WaitForJournal(
    const std::string& id, std::uint64_t after, std::chrono::milliseconds wait) const {
  std::unique_lock<std::mutex> service_lock(mutex_);
  auto found = store_.records.find(id);
  if (found == store_.records.end()) return {};
  auto record = found->second;
  changed_.wait_for(service_lock, wait, [&] {
    std::lock_guard<std::mutex> lock(record->mutex);
    return stopping_ || record->snapshot.latest_journal_sequence > after;
  });
  service_lock.unlock();
  std::vector<JournalEntry> result;
  std::lock_guard<std::mutex> lock(record->mutex);
  if (!record->events.empty() && after != 0 && after + 1 < record->events.front().sequence) {
    JournalEntry gap;
    gap.kind = JournalEntry::Kind::kGap;
    gap.sequence = record->events.front().sequence - 1;
    gap.timestamp = clock_.WallNow();
    gap.execution = record->snapshot;
    gap.gap_after_sequence = after;
    gap.gap_first_available_sequence = record->events.front().sequence;
    result.push_back(std::move(gap));
  }
  for (const auto& event : record->events)
    if (event.sequence > after) result.push_back(event);
  return result;
}

std::vector<MonitorUpdate> SkillManager::WaitForMonitor(
    const ExecutionSelector& selector, std::uint64_t after, std::chrono::milliseconds wait) const {
  auto updates = journald_.ReadMonitor(selector, after);
  if (!updates.empty()) return updates;
  std::unique_lock<std::mutex> lock(mutex_);
  changed_.wait_for(lock, wait, [&] { return stopping_ || journald_.monitor_sequence() > after; });
  lock.unlock();
  return journald_.ReadMonitor(selector, after);
}

std::optional<std::size_t> SkillManager::RunnableJobLocked() {
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    const auto& claims = pending_[i].claims;
    bool blocked = false;
    for (const auto& active : active_claims_)
      blocked |= detail::ClaimsConflict(claims, active.second);
    for (std::size_t earlier = 0; earlier < i; ++earlier)
      blocked |= detail::ClaimsConflict(claims, pending_[earlier].claims);
    if (!blocked) return i;
    if (pending_[i].record) {
      std::lock_guard<std::mutex> lock(pending_[i].record->mutex);
      if (pending_[i].record->snapshot.queue_reason != "claim.conflict") {
        pending_[i].record->snapshot.queue_reason = "claim.conflict";
        journald_.Publish(pending_[i].record->snapshot);
        changed_.notify_all();
      }
    }
  }
  return std::nullopt;
}

std::vector<std::shared_ptr<detail::Record>> SkillManager::ExpireQueuedLocked() {
  std::vector<std::shared_ptr<detail::Record>> expired;
  const auto now_mono = clock_.MonotonicNow();
  for (auto job = pending_.begin(); job != pending_.end();) {
    if (!job->record || !DeadlineExceeded(*job->record, now_mono)) {
      ++job;
      continue;
    }
    const auto& record = job->record;
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      FinalizeTerminalLocked(*record, ExecutionStatus::kTimedOut, "deadline.exceeded");
      expired.push_back(record);
    }
    job = pending_.erase(job);
  }
  return expired;
}

std::chrono::milliseconds SkillManager::TimeUntilNextDeadlineLocked() const {
  const auto now = clock_.MonotonicNow();
  std::optional<eos::lib::MonotonicTime> deadline;
  for (const auto& job : pending_)
    if (job.record && job.record->deadline && (!deadline || *job.record->deadline < *deadline))
      deadline = job.record->deadline;
  if (!deadline) return std::chrono::hours(1);
  if (*deadline <= now) return std::chrono::milliseconds(0);
  return std::max(std::chrono::milliseconds(1),
                   std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now));
}

void SkillManager::WorkerLoop() {
  while (true) {
    Job job;
    std::vector<std::shared_ptr<detail::Record>> expired;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (!stopping_) {
        expired = ExpireQueuedLocked();
        if (!expired.empty() || RunnableJobLocked().has_value()) break;
        work_available_.wait_for(lock, TimeUntilNextDeadlineLocked());
      }
      if (!expired.empty()) {
        lock.unlock();
        changed_.notify_all();
        for (const auto& record : expired) {
          CleanupTerminal(record, false);
          ExecutionSnapshot snapshot;
          {
            std::lock_guard<std::mutex> record_lock(record->mutex);
            snapshot = record->snapshot;
          }
          NotifyExecution(snapshot);
          LogExecutionEvent(spdlog::level::warn, "skill_execution_terminal",
                            snapshot.execution_id, snapshot.skill_name, snapshot.skill_version,
                            snapshot.request_id, ExecutionStatusName(snapshot.status),
                            snapshot.reason_code, snapshot.sequence);
        }
        continue;
      }
      if (stopping_ && pending_.empty()) return;
      auto index = RunnableJobLocked();
      if (!index) {
        if (stopping_) return;
        continue;
      }
      job = std::move(pending_[*index]);
      pending_.erase(pending_.begin() + *index);
      bool parent_inactive = false;
      std::string parent_id;
      {
        std::lock_guard<std::mutex> record_lock(job.record->mutex);
        parent_id = job.record->snapshot.parent_execution_id;
      }
      if (!parent_id.empty()) {
        const auto parent = store_.records.find(parent_id);
        if (parent == store_.records.end()) {
          parent_inactive = true;
        } else {
          std::lock_guard<std::mutex> parent_lock(parent->second->mutex);
          parent_inactive = IsTerminal(parent->second->snapshot.status) ||
                            parent->second->cancel_requested ||
                            DeadlineExceeded(*parent->second, clock_.MonotonicNow());
        }
      }
      if (parent_inactive) {
        {
          std::lock_guard<std::mutex> record_lock(job.record->mutex);
          FinalizeTerminalLocked(*job.record, ExecutionStatus::kCancelled,
                                 "parent.not_active");
        }
        expired.push_back(job.record);
        lock.unlock();
        for (const auto& record : expired) {
          CleanupTerminal(record, false);
          ExecutionSnapshot snapshot;
          {
            std::lock_guard<std::mutex> record_lock(record->mutex);
            snapshot = record->snapshot;
          }
          NotifyExecution(snapshot);
        }
        changed_.notify_all();
        continue;
      }
      active_claims_.push_back({job.record->snapshot.execution_id, job.claims});
      ++active_runs_;
    }
    std::optional<eos::lib::CorrelationScope> correlation;
    if (!job.correlation_id.empty()) correlation.emplace(std::move(job.correlation_id));
    Run(job.record, job.owner, std::move(job.arguments));
  }
}

void SkillManager::Run(const std::shared_ptr<detail::Record>& record, const OwnedSkill& owner,
                       nlohmann::json arguments) {
  auto transition = std::make_shared<std::function<void(ExecutionStatus, std::string)>>();
  *transition = [this, record](ExecutionStatus status, std::string reason = {}) {
    ExecutionSnapshot snapshot;
    nlohmann::json terminal_details = nlohmann::json::object();
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      const auto now_mono = clock_.MonotonicNow();
      const auto now_wall = clock_.WallNow();
      if (status == ExecutionStatus::kRunning) {
        record->started_monotonic = now_mono;
        record->snapshot.started_at = now_wall;
        record->snapshot.queue_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now_mono - record->created_monotonic);
      } else if (IsTerminal(status)) {
        record->finished_monotonic = now_mono;
        record->snapshot.finished_at = now_wall;
        record->snapshot.total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now_mono - record->created_monotonic);
        if (record->started_monotonic) {
          record->snapshot.run_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
              now_mono - *record->started_monotonic);
        }
        record->snapshot.current_activity.reset();
        record->active_activities.clear();
        record->activity_order.clear();
        for (auto event = record->events.rbegin(); event != record->events.rend(); ++event) {
          if (event->kind == JournalEntry::Kind::kRecord &&
              event->record.kind == JournalKind::kActivity &&
              event->record.outcome != "succeeded") {
            terminal_details["last_failed_activity"] = event->record.name;
            terminal_details["last_failed_activity_outcome"] = event->record.outcome;
            break;
          }
        }
        const auto dropped = record->snapshot.accounting.diagnostic_logs_dropped +
                             record->snapshot.accounting.checkpoints_dropped +
                             record->snapshot.accounting.activity_events_dropped;
        if (dropped != 0 && !record->suppression_logged) {
          JournalRecord suppression;
          suppression.event_id = lib::MintUuidV7();
          suppression.source = JournalSource::kSystem;
          suppression.kind = JournalKind::kSuppression;
          suppression.name = "observability.suppressed";
          suppression.outcome = "dropped";
          suppression.timestamp = now_wall;
          suppression.elapsed = record->snapshot.total_duration;
          suppression.fields = {
              {"diagnostic_logs", record->snapshot.accounting.diagnostic_logs_dropped},
              {"checkpoints", record->snapshot.accounting.checkpoints_dropped},
              {"activity_events", record->snapshot.accounting.activity_events_dropped}};
          (void)journald_.AppendRecord(*record, std::move(suppression), options_.max_journal_entries);
          record->suppression_logged = true;
        }
      }
      record->snapshot.status = status;
      record->snapshot.reason_code = std::move(reason);
      record->snapshot.queue_reason.clear();
      journald_.AppendLifecycle(*record, options_.max_journal_entries);
      snapshot = record->snapshot;
      journald_.Publish(snapshot);
    }
    changed_.notify_all();
    NotifyExecution(snapshot);
    auto level = spdlog::level::debug;
    if (status == ExecutionStatus::kRunning) level = spdlog::level::trace;
    if (status == ExecutionStatus::kTimedOut) level = spdlog::level::warn;
    if (status == ExecutionStatus::kFailed)
      level = snapshot.reason_code == "provider.exception" ||
                      snapshot.reason_code == "skill.protocol_error"
                  ? spdlog::level::err
                  : spdlog::level::warn;
    if (IsTerminal(status)) {
      const auto& accounting = snapshot.accounting;
      terminal_details.update({{"primitive_calls", accounting.primitive_calls},
                               {"resource_reads", accounting.resource_reads},
                               {"signals_received", accounting.signals_received},
                               {"signals_emitted", accounting.signals_emitted},
                               {"diagnostic_logs", accounting.diagnostic_logs},
                               {"diagnostic_logs_dropped", accounting.diagnostic_logs_dropped},
                               {"checkpoints", accounting.checkpoints},
                               {"checkpoints_dropped", accounting.checkpoints_dropped},
                               {"queue_duration_ms", snapshot.queue_duration.count()},
                               {"run_duration_ms", snapshot.run_duration.count()},
                               {"total_duration_ms", snapshot.total_duration.count()}});
    }
    LogExecutionEvent(level,
                      status == ExecutionStatus::kRunning ? "skill_execution_started"
                                                          : "skill_execution_terminal",
                      snapshot.execution_id, snapshot.skill_name, snapshot.skill_version,
                      snapshot.request_id, ExecutionStatusName(status), snapshot.reason_code,
                      snapshot.sequence, snapshot.message, terminal_details);
    if (IsTerminal(status)) {
      const auto& accounting = snapshot.accounting;
      LogSkillEvent(spdlog::level::debug, "skill_execution_accounting",
                    {{"execution_id", snapshot.execution_id},
                     {"primitive_calls", accounting.primitive_calls},
                     {"resource_reads", accounting.resource_reads},
                     {"signals_received", accounting.signals_received},
                     {"signals_emitted", accounting.signals_emitted},
                     {"diagnostic_logs", accounting.diagnostic_logs},
                     {"diagnostic_logs_dropped", accounting.diagnostic_logs_dropped},
                     {"checkpoints", accounting.checkpoints},
                     {"checkpoints_dropped", accounting.checkpoints_dropped},
                     {"activity_events_dropped", accounting.activity_events_dropped},
                     {"queue_duration_ms", snapshot.queue_duration.count()},
                     {"run_duration_ms", snapshot.run_duration.count()},
                     {"total_duration_ms", snapshot.total_duration.count()}});
    }
  };
  bool cancelled = false, timed_out = false;
  {
    std::lock_guard<std::mutex> lock(record->mutex);
    cancelled = record->cancel_requested;
    timed_out = !cancelled && DeadlineExceeded(*record, clock_.MonotonicNow());
  }
  if (cancelled) {
    (*transition)(ExecutionStatus::kCancelled, "cancelled");
    CleanupTerminal(record, true);
    return;
  }
  if (timed_out) {
    (*transition)(ExecutionStatus::kTimedOut, "deadline.exceeded");
    CleanupTerminal(record, true);
    return;
  }
  (*transition)(ExecutionStatus::kRunning, {});
  ExecutionHooks hooks;
  hooks.execution_id = record->snapshot.execution_id;
  hooks.runtime_id = owner.runtime_id;
  hooks.parent_process = record->parent_process;
  hooks.cancelled = [this, record] {
    std::lock_guard<std::mutex> lock(record->mutex);
    return record->cancel_requested || DeadlineExceeded(*record, clock_.MonotonicNow());
  };
  hooks.publish_execution_state = [this, record, schema = owner.metadata->execution_state_schema](
                                      nlohmann::json payload) {
    return PublishState(record, schema, std::move(payload), false);
  };
  hooks.publish_related_process_state =
      [this, record, schema = owner.metadata->related_process_state_schema](nlohmann::json payload) {
        return PublishState(record, schema, std::move(payload), true);
      };
  hooks.read_related_process_state = [this, requester = record->snapshot.execution_id](
                                         const ProcessHandle& process) {
    return GetRelatedProcessState(requester, process);
  };
  hooks.wait_related_process_state = [this, requester = record->snapshot.execution_id](
                                         const ProcessHandle& process,
                                         std::uint64_t after_revision,
                                         std::chrono::milliseconds timeout) {
    return WaitForRelatedProcessState(requester, process, after_revision, timeout);
  };
  auto begin_activity = [this, record](const Activity& activity) {
    const std::string id = lib::MintUuidV7();
    const auto now_mono = clock_.MonotonicNow();
    const auto now_wall = clock_.WallNow();
    std::lock_guard<std::mutex> lock(record->mutex);
    record->activity_started[id] = now_mono;
    auto view = ExecutionSnapshot::ActivityView{
        id, activity.name, now_wall, eos::lib::SanitizeLogFields(activity.fields)};
    record->active_activities[id] = view;
    record->activity_order.push_back(id);
    record->snapshot.current_activity = view;
    if (activity.name == "primitive.call") ++record->snapshot.accounting.primitive_calls;
    if (activity.name == "resource.read") ++record->snapshot.accounting.resource_reads;
    JournalRecord journal_record{
        id,
        JournalSource::kSystem,
        JournalKind::kActivity,
        activity.name,
        "started",
        {},
        std::nullopt,
        view.fields,
        now_wall,
        std::chrono::duration_cast<std::chrono::milliseconds>(now_mono - record->created_monotonic),
        std::nullopt};
    const bool history_reserved = record->events.size() + 4 >= options_.max_journal_entries;
    if (record->automatic_activity_events >= 512 || history_reserved ||
        !journald_.AppendRecord(*record, std::move(journal_record), options_.max_journal_entries)) {
      ++record->snapshot.accounting.activity_events_dropped;
    } else {
      ++record->automatic_activity_events;
      ++record->snapshot.accounting.activity_events;
      changed_.notify_all();
    }
    journald_.Publish(record->snapshot);
    changed_.notify_all();
    return id;
  };
  auto end_activity = [this, record](const std::string& id, const std::string& outcome) {
    const auto now_mono = clock_.MonotonicNow();
    const auto now_wall = clock_.WallNow();
    std::lock_guard<std::mutex> lock(record->mutex);
    const auto found = record->activity_started.find(id);
    if (found == record->activity_started.end()) return;
    const auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(now_mono - found->second);
    const auto active = record->active_activities.find(id);
    const std::string name =
        active == record->active_activities.end() ? "activity" : active->second.name;
    nlohmann::json fields = active == record->active_activities.end()
                                ? nlohmann::json::object()
                                : active->second.fields;
    if (name == "claim.wait") record->snapshot.accounting.claim_wait_duration += duration;
    if (name == "signal.wait") record->snapshot.accounting.signal_wait_duration += duration;
    if (name == "primitive.call" || name == "resource.read")
      record->snapshot.accounting.capability_duration += duration;
    record->active_activities.erase(id);
    record->activity_order.erase(
        std::remove(record->activity_order.begin(), record->activity_order.end(), id),
        record->activity_order.end());
    record->snapshot.current_activity.reset();
    if (!record->activity_order.empty()) {
      const auto remaining = record->active_activities.find(record->activity_order.back());
      if (remaining != record->active_activities.end())
        record->snapshot.current_activity = remaining->second;
    }
    record->activity_started.erase(found);
    JournalRecord journal_record{
        id,
        JournalSource::kSystem,
        JournalKind::kActivity,
        name,
        outcome,
        {},
        std::nullopt,
        std::move(fields),
        now_wall,
        std::chrono::duration_cast<std::chrono::milliseconds>(now_mono - record->created_monotonic),
        duration};
    const bool history_reserved = record->events.size() + 4 >= options_.max_journal_entries;
    if (record->automatic_activity_events >= 512 || history_reserved ||
        !journald_.AppendRecord(*record, std::move(journal_record), options_.max_journal_entries)) {
      ++record->snapshot.accounting.activity_events_dropped;
    } else {
      ++record->automatic_activity_events;
      ++record->snapshot.accounting.activity_events;
      changed_.notify_all();
    }
    journald_.Publish(record->snapshot);
    changed_.notify_all();
  };
  hooks.begin_activity = begin_activity;
  hooks.end_activity = end_activity;
  hooks.record_capability = [this, record](const std::string& name,
                                           std::chrono::milliseconds duration) {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (name == "primitive.call") ++record->snapshot.accounting.primitive_calls;
    if (name == "resource.read") ++record->snapshot.accounting.resource_reads;
    record->snapshot.accounting.capability_duration += duration;
    journald_.Publish(record->snapshot);
    changed_.notify_all();
  };
  hooks.diagnostic_log = [this, record](bool accepted) {
    std::lock_guard<std::mutex> lock(record->mutex);
    if (accepted)
      ++record->snapshot.accounting.diagnostic_logs;
    else
      ++record->snapshot.accounting.diagnostic_logs_dropped;
    journald_.Publish(record->snapshot);
    changed_.notify_all();
  };
  hooks.emit_checkpoint = [this, record](const Checkpoint& checkpoint) {
    const auto now_mono = clock_.MonotonicNow();
    const auto now_wall = clock_.WallNow();
    std::lock_guard<std::mutex> lock(record->mutex);
    // Leave room for terminal output signals, their activity accounting, and
    // the authoritative terminal lifecycle. Observability must not make an
    // otherwise valid execution fail closed at the SIGNAL boundary.
    const bool history_reserved = record->events.size() + 4 >= options_.max_journal_entries;
    if (IsTerminal(record->snapshot.status) || record->checkpoints_accepted >= 256 ||
        history_reserved) {
      ++record->snapshot.accounting.checkpoints_dropped;
      journald_.Publish(record->snapshot);
      changed_.notify_all();
      return false;
    }
    JournalRecord journal_record{
        lib::MintUuidV7(),
        JournalSource::kSkill,
        JournalKind::kCheckpoint,
        checkpoint.name,
        "emitted",
        checkpoint.message,
        checkpoint.progress,
        eos::lib::SanitizeLogFields(checkpoint.fields),
        now_wall,
        std::chrono::duration_cast<std::chrono::milliseconds>(now_mono - record->created_monotonic),
        std::nullopt};
    if (!journald_.AppendRecord(*record, std::move(journal_record), options_.max_journal_entries)) {
      ++record->snapshot.accounting.checkpoints_dropped;
      journald_.Publish(record->snapshot);
      changed_.notify_all();
      return false;
    }
    ++record->checkpoints_accepted;
    ++record->snapshot.accounting.checkpoints;
    journald_.Publish(record->snapshot);
    changed_.notify_all();
    return true;
  };
  hooks.acquire_device = [this, record, cancelled = hooks.cancelled, begin_activity,
                          end_activity](const std::string& device_id) -> std::function<void()> {
    if (device_id.empty()) return [] {};
    const std::string activity_id = begin_activity({"claim.wait", {{"kind", "device"}}});
    const std::vector<ResolvedClaim> requested = {
        {ClaimKind::kDevice, ClaimMode::kExclusiveUse, device_id}};
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      if (stopping_ || cancelled()) {
        end_activity(activity_id, "cancelled");
        return {};
      }
      bool conflict = false;
      for (const auto& active : active_claims_)
        if (active.first != record->snapshot.execution_id &&
            detail::ClaimsConflict(requested, active.second)) {
          conflict = true;
          break;
        }
      if (!conflict) break;
      work_available_.wait_for(lock, std::chrono::milliseconds(20));
    }
    end_activity(activity_id, "acquired");
    const std::string lease_id = lib::MintUuidV7();
    active_claims_.push_back({lease_id, requested});
    {
      std::lock_guard<std::mutex> record_lock(record->mutex);
      record->snapshot.active_device_claims = requested;
      journald_.AppendLifecycle(*record, options_.max_journal_entries);
      journald_.Publish(record->snapshot);
    }
    changed_.notify_all();
    return [this, record, lease_id] {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_claims_.erase(
            std::remove_if(active_claims_.begin(), active_claims_.end(),
                           [&](const auto& active) { return active.first == lease_id; }),
            active_claims_.end());
        std::lock_guard<std::mutex> record_lock(record->mutex);
        record->snapshot.active_device_claims.clear();
        journald_.AppendLifecycle(*record, options_.max_journal_entries);
        journald_.Publish(record->snapshot);
      }
      changed_.notify_all();
      work_available_.notify_all();
    };
  };
  hooks.receive_signal =
      [this, record, begin_activity, end_activity](
          std::chrono::milliseconds wait) -> std::optional<eos::runtime::skills::Signal> {
    const std::string activity_id =
        wait > std::chrono::milliseconds::zero()
            ? begin_activity({"signal.wait", {{"timeout_ms", wait.count()}}})
            : std::string{};
    std::unique_lock<std::mutex> lock(record->mutex);
    record->signal_available.wait_for(
        lock, wait, [&] { return record->cancel_requested || !record->inbound.empty(); });
    if (record->cancel_requested && !record->cancel_delivered) {
      record->cancel_delivered = true;
      lock.unlock();
      end_activity(activity_id, "cancelled");
      return eos::runtime::skills::Signal{"CANCEL", nlohmann::json::object(), "",
                                          record->snapshot.execution_id};
    }
    if (record->inbound.empty()) {
      lock.unlock();
      end_activity(activity_id, "timeout");
      return std::nullopt;
    }
    eos::runtime::skills::Signal signal = std::move(record->inbound.front().signal);
    record->inbound.pop_front();
    ++record->snapshot.accounting.signals_received;
    journald_.Publish(record->snapshot);
    changed_.notify_all();
    lock.unlock();
    end_activity(activity_id, "received");
    return signal;
  };
  hooks.emit_signal = [this, record, owner, begin_activity,
                       end_activity](const eos::runtime::skills::Signal& signal) {
    if (!owner.metadata->supports_signals) return false;
    const auto* declaration = FindSignal(owner.metadata->emitted_signals, signal.name);
    if (!declaration) return false;
    try {
      runtimes_.ValidateSignal(owner, *declaration, signal.payload);
    } catch (...) {
      return false;
    }
    const std::string activity_id = begin_activity({"signal.emit", {{"signal", signal.name}}});
    eos::runtime::skills::Signal emitted = signal;
    if (emitted.signal_id.empty()) emitted.signal_id = lib::MintUuidV7();
    emitted.execution_id = record->snapshot.execution_id;
    std::string emitted_signal_id;
    std::string execution_id;
    std::uint64_t emitted_sequence = 0;
    ExecutionSnapshot observed_execution;
    eos::runtime::skills::Signal observed_signal;
    bool appended = false;
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      if (!IsTerminal(record->snapshot.status) &&
          journald_.AppendSignal(*record, std::move(emitted), clock_.WallNow(),
                                 options_.max_journal_entries)) {
        appended = true;
        emitted_signal_id = record->events.back().signal.signal_id;
        execution_id = record->snapshot.execution_id;
        emitted_sequence = record->snapshot.latest_journal_sequence;
        observed_execution = record->snapshot;
        observed_signal = record->events.back().signal;
        ++record->snapshot.accounting.signals_emitted;
        journald_.Publish(record->snapshot);
      }
    }
    if (!appended) {
      end_activity(activity_id, "unavailable");
      return false;
    }
    LogSkillEvent(spdlog::level::trace, "skill_signal_emitted",
                  {{"execution_id", execution_id},
                   {"signal", signal.name},
                   {"signal_id", emitted_signal_id},
                   {"sequence", emitted_sequence}});
    changed_.notify_all();
    NotifySignal(observed_execution, observed_signal);
    end_activity(activity_id, "emitted");
    return true;
  };
  const auto source_authorized = [owner](const std::string& topic, SourceAuthority authority) {
    return std::any_of(owner.metadata->sources.begin(), owner.metadata->sources.end(),
                       [&](const SourceDescriptor& source) {
                         return source.topic == topic && source.authority == authority;
                       });
  };
  hooks.publish_source = [this, source_authorized](const std::string& topic,
                                                   std::int64_t timestamp,
                                                   nlohmann::json value) {
    if (!source_authorized(topic, SourceAuthority::kWrite))
      throw std::invalid_argument("skill has no write authority for source topic");
    return sources_.Publish(topic, timestamp, std::move(value));
  };
  hooks.query_source = [this, source_authorized](const std::string& topic, SourceLookup lookup,
                                                 std::int64_t timestamp) {
    if (!source_authorized(topic, SourceAuthority::kRead))
      throw std::invalid_argument("skill has no read authority for source topic");
    return sources_.Query(topic, lookup, timestamp);
  };
  hooks.wait_source = [this, source_authorized, cancelled = hooks.cancelled](
                          const std::string& topic, std::uint64_t after,
                          std::chrono::milliseconds timeout) {
    if (!source_authorized(topic, SourceAuthority::kRead))
      throw std::invalid_argument("skill has no read authority for source topic");
    return sources_.WaitForNewer(topic, after, timeout, cancelled);
  };
  hooks.spawn = [this, parent = record->snapshot.execution_id](
                    const std::string& name, const std::string& version,
                    nlohmann::json arguments, const std::string& request_id,
                    std::chrono::milliseconds timeout) {
    const auto started = SpawnChild(parent, name, version, std::move(arguments), request_id,
                                    StartOptions{timeout});
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = store_.records.find(started.execution.execution_id);
    if (found == store_.records.end()) throw std::runtime_error("spawned process was not retained");
    std::lock_guard<std::mutex> record_lock(found->second->mutex);
    return ProcessHandle{found->second->snapshot.execution_id,
                         found->second->snapshot.parent_execution_id,
                         found->second->process_generation};
  };
  hooks.send_message = [this, sender = record->snapshot.execution_id](
                           const ProcessHandle& recipient, const std::string& channel,
                           nlohmann::json payload, const std::string& request_id,
                           std::chrono::milliseconds ttl) {
    return SendMessage(sender, recipient, channel, std::move(payload), request_id, ttl);
  };
  hooks.receive_message = [this, execution_id = record->snapshot.execution_id](
                               const std::string& channel, std::chrono::milliseconds timeout) {
    return ReceiveMessage(execution_id, channel, timeout);
  };
  hooks.receive_reply = [this, execution_id = record->snapshot.execution_id](
                            const ProcessHandle& process, const std::string& channel,
                            const std::string& correlation_id, std::chrono::milliseconds timeout) {
    return ReceiveMessage(execution_id, channel, timeout, correlation_id,
                          process.execution_id);
  };
  hooks.await_exit = [this, parent = record->snapshot.execution_id](
                         const ProcessHandle& child, std::chrono::milliseconds timeout) {
    return AwaitExit(parent, child, timeout);
  };
  hooks.terminate_process = [this, parent = record->snapshot.execution_id](
                                const ProcessHandle& child) {
    return TerminateChild(parent, child);
  };
  auto complete = [this, record, transition](RunResult result) mutable {
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      if (record->provider_completed) return;
      record->provider_completed = true;
    }
    ExecutionStatus terminal = ExecutionStatus::kFailed;
    std::string reason;
    {
      std::lock_guard<std::mutex> lock(record->mutex);
      if (DeadlineExceeded(*record, clock_.MonotonicNow()))
        terminal = ExecutionStatus::kTimedOut;
      else if (result.status == RunResult::Status::kCancelled || record->cancel_requested)
        terminal = ExecutionStatus::kCancelled;
      else if (result.status == RunResult::Status::kCompleted && result.accepted)
        terminal = ExecutionStatus::kSucceeded;
      else if (result.status == RunResult::Status::kCompleted)
        terminal = ExecutionStatus::kRejected;
      record->snapshot.result = std::move(result.result);
      record->snapshot.primitive_acks = std::move(result.primitive_acks);
      record->snapshot.message = std::move(result.message);
      reason = terminal == ExecutionStatus::kTimedOut
                   ? "deadline.exceeded"
                   : (terminal == ExecutionStatus::kCancelled
                          ? "cancelled"
                          : (result.reason_code.empty() ? ExecutionStatusName(terminal)
                                                        : result.reason_code));
    }
    (*transition)(terminal, std::move(reason));
    CleanupTerminal(record, true);
  };
  try {
    owner.provider->ExecuteAsync(record->snapshot.skill_name, record->snapshot.skill_version,
                                 arguments, record->snapshot.request_id, std::move(hooks),
                                 complete);
  } catch (const std::exception& error) {
    RunResult result;
    result.status = RunResult::Status::kInternal;
    result.reason_code = "provider.exception";
    result.message = error.what();
    complete(std::move(result));
  } catch (...) {
    RunResult result;
    result.status = RunResult::Status::kInternal;
    result.reason_code = "provider.exception";
    complete(std::move(result));
  }
}

void SkillManager::NotifyExecution(const ExecutionSnapshot& execution) const noexcept {
  if (!observers_.execution) return;
  try {
    observers_.execution(execution);
  } catch (...) {
    LogSkillEvent(spdlog::level::err, "skill_observer_failed",
                  {{"observer", "execution"},
                   {"execution_id", execution.execution_id},
                   {"status", ExecutionStatusName(execution.status)}});
  }
}

void SkillManager::NotifySignal(const ExecutionSnapshot& execution,
                                const eos::runtime::skills::Signal& signal) const noexcept {
  if (!observers_.emitted_signal) return;
  try {
    observers_.emitted_signal(execution, signal);
  } catch (...) {
    LogSkillEvent(spdlog::level::err, "skill_observer_failed",
                  {{"observer", "emitted_signal"},
                   {"execution_id", execution.execution_id},
                   {"signal", signal.name}});
  }
}

}  // namespace eos::runtime::skills

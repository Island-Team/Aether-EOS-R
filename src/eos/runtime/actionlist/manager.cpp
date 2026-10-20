#include "src/eos/runtime/actionlist/manager.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"
#include "src/eos/runtime/actionlist/journal.h"

namespace eos::runtime::actionlist {
namespace {
using Clock = std::chrono::system_clock;

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("actionlist");
  return logger;
}

std::int64_t UnixMillis(Clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

bool TerminalAction(const std::string& status) {
  return status == "succeeded" || status == "rejected" || status == "failed" ||
         status == "cancelled" || status == "timed_out" || status == "outcome_unknown" ||
         status == "skipped" || status == "stopped";
}

bool SuccessfulAction(const std::string& status) {
  return status == "succeeded" || status == "skipped" || status == "stopped";
}

bool FailedAction(const std::string& status) {
  return status == "rejected" || status == "failed" || status == "cancelled" ||
         status == "timed_out" || status == "outcome_unknown";
}

std::vector<std::string> ParseJsonPointer(const std::string& pointer) {
  if (pointer.empty()) return {};
  if (pointer.front() != '/')
    throw std::invalid_argument("JSON Pointer must be empty or start with /");
  std::vector<std::string> tokens;
  std::size_t start = 1;
  while (true) {
    const std::size_t end = pointer.find('/', start);
    std::string token;
    const std::string_view encoded(pointer.data() + start,
                                   (end == std::string::npos ? pointer.size() : end) - start);
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      if (encoded[index] != '~') {
        token.push_back(encoded[index]);
        continue;
      }
      if (++index == encoded.size() || (encoded[index] != '0' && encoded[index] != '1'))
        throw std::invalid_argument("JSON Pointer contains an invalid escape");
      token.push_back(encoded[index] == '0' ? '~' : '/');
    }
    tokens.push_back(std::move(token));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return tokens;
}

bool PointerOverlaps(const std::vector<std::string>& left,
                      const std::vector<std::string>& right) {
  const std::size_t common = std::min(left.size(), right.size());
  return std::equal(left.begin(), left.begin() + common, right.begin());
}

bool DetectionValuePointerAllowed(const std::string& pointer) {
  static const std::set<std::string> kAllowed = {
      "/id",                         "/label",
      "/confidence",                 "/bbox",
      "/distance_m",                 "/bearing_rad",
      "/attributes/name",            "/attributes/pose",
      "/attributes/pose_confidence", "/attributes/posture_state",
      "/attributes/action",          "/attributes/action_confidence",
      "/attributes/activity",        "/attributes/speaking"};
  return kAllowed.count(pointer) != 0;
}

const nlohmann::json& FindAction(const nlohmann::json& list, const std::string& action_id) {
  for (const auto& action : list.at("actions"))
    if (action.at("action_id") == action_id) return action;
  throw std::logic_error("binding source action is missing");
}

bool DependencySatisfied(const nlohmann::json& list, const nlohmann::json& action,
                         const std::string& dependency_id) {
  const auto& dependency = FindAction(list, dependency_id);
  const auto status = dependency.at("status").get<std::string>();
  if (action.value("finalizer", false)) return TerminalAction(status);
  for (const auto& binding : action.value("output_bindings", nlohmann::json::array()))
    if (binding.at("source_action_id") == dependency_id) return status == "succeeded";
  if (dependency.value("execution_mode", std::string()) == "monitor")
    return status == "queued" || status == "running" || status == "stopped" ||
           status == "succeeded";
  return SuccessfulAction(status) ||
         (dependency.value("continue_on_failure", false) && FailedAction(status));
}

nlohmann::json ResolveParameters(const nlohmann::json& list, const nlohmann::json& action) {
  nlohmann::json resolved = action.at("parameters");
  for (const auto& binding : action.value("output_bindings", nlohmann::json::array())) {
    const auto& source = FindAction(list, binding.at("source_action_id").get<std::string>());
    if (source.at("status") != "succeeded" || !source.contains("output"))
      throw std::logic_error("binding source action has not succeeded with output");
    try {
      const auto& value = source.at("output").at(
          nlohmann::json::json_pointer(binding.at("source_output_pointer").get<std::string>()));
      resolved.at(nlohmann::json::json_pointer(
          binding.at("destination_parameter_pointer").get<std::string>())) = value;
    } catch (const nlohmann::json::exception&) {
      throw std::invalid_argument("binding JSON Pointer does not resolve");
    }
  }
  if (!resolved.is_object())
    throw std::invalid_argument("resolved action parameters must be an object");
  return resolved;
}

void Log(spdlog::level::level_enum level, const char* event, const std::string& workflow_id,
         const std::string& action_id = {}, const std::string& status = {},
         std::uint64_t revision = 0, const std::string& reason_code = {}) {
  nlohmann::json fields{{"workflow_id", workflow_id}};
  if (!action_id.empty()) {
    fields["action_id"] = action_id;
    fields["action_ref"] = workflow_id + "/actions/" + action_id;
  }
  if (!status.empty()) fields["status"] = status;
  if (revision != 0) fields["revision"] = revision;
  std::string summary;
  if (level >= spdlog::level::warn && !action_id.empty()) {
    summary = "ActionList " + workflow_id + " action " + action_id + " changed to " + status;
    if (reason_code == "human.owner_not_found") {
      summary += ": no stable detection matching the required owner identity, action, and pose "
                 "was found within 60 seconds";
    } else if (!reason_code.empty()) {
      summary += " (reason: " + reason_code + ")";
    }
  }
  eos::lib::LogEvent(Logger(), level, event, fields,
                     summary);
}
}  // namespace

class Manager::JournalOwner final {
 public:
  JournalOwner(const std::filesystem::path& directory, Journal::Limits limits)
      : repository(directory, limits) {}
  Journal repository;
};

Manager::Manager(SkillRunner skill_runner, PrimitiveRunner primitive_runner, ManagerOptions options)
    : skill_runner_(std::move(skill_runner)),
      primitive_runner_(std::move(primitive_runner)),
      event_epoch_(lib::MintUuidV7()),
      max_events_(options.max_events),
      max_workflows_(options.max_workflows),
      max_workflow_bytes_(options.max_workflow_bytes),
      max_actions_per_workflow_(options.max_actions_per_workflow),
      read_resource_(std::move(options.read_resource)) {
  if (!skill_runner_.start || !skill_runner_.cancel || !skill_runner_.list_executions)
    throw std::invalid_argument("ActionList skill runner callbacks are required");
  if (options.worker_count == 0 || options.max_events == 0 || options.max_workflows == 0 ||
      options.max_workflow_bytes == 0 || options.max_actions_per_workflow == 0 ||
      options.journal_max_record_bytes == 0 || options.journal_max_bytes == 0)
    throw std::invalid_argument(
        "ActionList worker count and event retention must be greater than zero");
  if (!options.journal_directory.empty()) {
    journal_ = std::make_unique<JournalOwner>(
        options.journal_directory,
        Journal::Limits{options.journal_max_record_bytes, options.journal_max_bytes});
    RecoverJournal();
  }
  for (std::size_t i = 0; i < options.worker_count; ++i)
    workers_.emplace_back(&Manager::WorkerLoop, this);
}

Manager::~Manager() {
  std::vector<std::string> commands;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    for (const auto& [command, cancelled] : primitive_cancellations_) {
      cancelled->store(true);
      commands.push_back(command);
    }
  }
  work_available_.notify_all();
  events_changed_.notify_all();
  if (primitive_runner_.cancel) {
    for (const auto& command : commands) {
      try {
        primitive_runner_.cancel(command);
      } catch (const std::exception&) {
      }
    }
  }
  for (auto& worker : workers_)
    if (worker.joinable()) worker.join();
}

void Manager::WorkerLoop() {
  while (true) {
    PrimitiveJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_available_.wait(lock, [&] { return stopping_ || !pending_.empty(); });
      if (stopping_ && pending_.empty()) return;
      job = std::move(pending_.front());
      pending_.pop_front();
    }
    std::optional<eos::lib::CorrelationScope> correlation;
    if (!job.correlation_id.empty()) correlation.emplace(std::move(job.correlation_id));
    try {
      RunPrimitive(std::move(job));
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!journal_failed_) throw;
    }
  }
}

void Manager::ObservePrimitive(const std::string& workflow_id, const std::string& action_id,
                               const std::string& status, nlohmann::json result,
                               const std::string& reason) {
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (journal_failed_) return;
    auto workflow = action_lists_.find(workflow_id);
    if (workflow == action_lists_.end()) return;
    for (auto& action : workflow->second.action_list["actions"]) {
      if (action.at("action_id") != action_id || !action.contains("primitive") ||
          TerminalAction(action.at("status").get<std::string>()))
        continue;
      const std::string applied_status =
          status == "cancelled" && action.value("stop_requested", false) &&
                  action.value("execution_mode", std::string()) == "monitor"
              ? "stopped"
              : status;
      action["status"] = applied_status;
      if (!reason.empty()) action["reason"] = reason;
      if (TerminalAction(applied_status)) {
        action["output"] = result.is_object() && result.contains("result")
                               ? result.at("result")
                               : nlohmann::json(nullptr);
        action["result"] = std::move(result);
      }
      auto& list = workflow->second.action_list;
      list["revision"] = list["revision"].get<std::uint64_t>() + 1;
      list["updated_at_unix_ms"] = UnixMillis(Clock::now());
      Log(TerminalAction(applied_status) && !SuccessfulAction(applied_status)
              ? spdlog::level::warn
              : spdlog::level::debug,
          "action_list_action_transition", workflow_id, action_id, applied_status,
          list["revision"].get<std::uint64_t>());
      Advance(workflow->second);
      PublishActionListLocked(list);
      changed = true;
      break;
    }
  }
  if (changed) DrainDispatches();
}

void Manager::CancelPrimitive(const std::string& command_id) {
  bool queued = false;
  std::string workflow_id;
  std::string action_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto cancellation = primitive_cancellations_.find(command_id);
    if (cancellation == primitive_cancellations_.end()) return;
    cancellation->second->store(true);
    auto pending = std::find_if(pending_.begin(), pending_.end(), [&](const PrimitiveJob& job) {
      return job.command_id == command_id;
    });
    if (pending != pending_.end()) {
      workflow_id = pending->workflow_id;
      action_id = pending->action_id;
      pending_.erase(pending);
      primitive_cancellations_.erase(cancellation);
      queued = true;
    }
  }
  work_available_.notify_all();
  if (queued) {
    ObservePrimitive(workflow_id, action_id, "cancelled", nlohmann::json::object(), "cancelled");
  } else if (primitive_runner_.cancel) {
    try {
      primitive_runner_.cancel(command_id);
    } catch (const std::exception&) {
    }
  }
}

void Manager::RunPrimitive(PrimitiveJob job) {
  if (job.cancel_requested->load()) {
    ObservePrimitive(job.workflow_id, job.action_id, "cancelled", nlohmann::json::object(),
                     "cancelled");
  } else {
    ObservePrimitive(job.workflow_id, job.action_id, "running");
    try {
      auto cancelled = [requested = job.cancel_requested] { return requested->load(); };
      nlohmann::json ack =
          primitive_runner_.execute(job.primitive_name, job.primitive_version, job.device_id,
                                    std::move(job.arguments), job.command_id, cancelled);
      std::string status = "outcome_unknown";
      std::string reason;
      if (ack.is_object() && ack.contains("result") && ack.at("result").is_object()) {
        status = ack.at("result").value("status", status);
        if (ack.at("result").contains("reason") && ack.at("result").at("reason").is_string())
          reason = ack.at("result").at("reason").get<std::string>();
        if (status == "rejected" && ack.contains("reason") && ack.at("reason").is_string())
          reason = ack.at("reason").get<std::string>();
      }
      if (!TerminalAction(status)) status = "outcome_unknown";
      ObservePrimitive(job.workflow_id, job.action_id, status, std::move(ack), reason);
    } catch (const std::exception& error) {
      ObservePrimitive(job.workflow_id, job.action_id, "outcome_unknown", nlohmann::json::object(),
                       error.what());
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  primitive_cancellations_.erase(job.command_id);
}

bool Manager::TokenCurrentLocked(const SkillOwner& owner) const {
  const auto workflow = action_lists_.find(owner.workflow_id);
  if (workflow == action_lists_.end()) return false;
  const auto token = workflow->second.action_tokens.find(owner.action_id);
  return token != workflow->second.action_tokens.end() && token->second == owner.token;
}

void Manager::ApplyExecutionLocked(const SkillOwner& owner,
                                   const skills::ExecutionSnapshot& execution) {
  if (!TokenCurrentLocked(owner)) return;
  auto workflow = action_lists_.find(owner.workflow_id);
  for (auto& action : workflow->second.action_list["actions"]) {
    if (action.at("action_id") != owner.action_id ||
        TerminalAction(action.at("status").get<std::string>()))
      continue;
    const std::string applied_status = execution.status == skills::ExecutionStatus::kCancelled &&
                                               action.value("stop_requested", false) &&
                                               action.value("execution_mode", std::string()) ==
                                                   "monitor"
                                           ? "stopped"
                                           : skills::ExecutionStatusName(execution.status);
    const bool changed = action.value("status", std::string()) != applied_status ||
                         action.value("execution_id", std::string()) != execution.execution_id ||
                         action.value("message", std::string()) != execution.message ||
                         (skills::IsTerminal(execution.status) &&
                          action.value("result", nlohmann::json::object()) != execution.result);
    if (!changed) return;
    action["status"] = applied_status;
    action["execution_id"] = execution.execution_id;
    if (!execution.reason_code.empty()) action["reason"] = execution.reason_code;
    if (execution.message.empty())
      action.erase("message");
    else
      action["message"] = execution.message;
    if (skills::IsTerminal(execution.status)) {
      action["result"] = execution.result;
      action["output"] = execution.result;
    }
    auto& list = workflow->second.action_list;
    list["revision"] = list["revision"].get<std::uint64_t>() + 1;
    list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    Log(skills::IsTerminal(execution.status) && !SuccessfulAction(applied_status)
            ? spdlog::level::warn
            : spdlog::level::debug,
        "action_list_action_transition", owner.workflow_id, owner.action_id, applied_status,
        list["revision"].get<std::uint64_t>(), execution.reason_code);
    Advance(workflow->second);
    PublishActionListLocked(list);
    return;
  }
}

void Manager::CancelSkillNoexcept(const std::string& execution_id) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++skill_cancellations_in_flight_[execution_id];
  }
  bool cancelled = false;
  try {
    cancelled = skill_runner_.cancel(execution_id);
  } catch (const std::exception&) {
    eos::lib::LogEvent(Logger(), spdlog::level::err, "action_list_skill_cancel_failed",
                       {{"execution_id", execution_id}, {"reason", "exception"}});
  } catch (...) {
    eos::lib::LogEvent(Logger(), spdlog::level::err, "action_list_skill_cancel_failed",
                       {{"execution_id", execution_id}, {"reason", "unknown"}});
  }
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    auto in_flight = skill_cancellations_in_flight_.find(execution_id);
    if (in_flight != skill_cancellations_in_flight_.end() && --in_flight->second == 0)
      skill_cancellations_in_flight_.erase(in_flight);
    if (!cancelled) {
      const auto owner = execution_owners_.find(execution_id);
      if (owner != execution_owners_.end() && TokenCurrentLocked(owner->second)) {
        skills::ExecutionSnapshot failed;
        failed.execution_id = execution_id;
        failed.status = skills::ExecutionStatus::kFailed;
        failed.reason_code = "cancellation.request_failed";
        failed.result = nlohmann::json::object();
        ApplyExecutionLocked(owner->second, failed);
      }
    }
  } catch (...) {
    eos::lib::LogEvent(Logger(), spdlog::level::err, "action_list_skill_cancel_reconcile_failed",
                       {{"execution_id", execution_id}});
  }
}

void Manager::DrainSkillStarts() {
  while (true) {
    SkillStartJob job;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_skill_starts_.empty()) return;
      job = std::move(pending_skill_starts_.front());
      pending_skill_starts_.pop_front();
      const auto pending = pending_skill_owners_.find(job.request_id);
      if (pending == pending_skill_owners_.end() ||
          pending->second.owner.token != job.owner.token || !TokenCurrentLocked(job.owner))
        continue;
    }

    std::optional<skills::StartResult> started;
    std::string failure;
    bool start_failed = false;
    try {
      started = skill_runner_.start(job.skill_name, job.skill_version, std::move(job.arguments),
                                    job.request_id);
    } catch (const std::exception& error) {
      start_failed = true;
      failure = error.what();
    } catch (...) {
      start_failed = true;
      failure = "skill start failed";
    }

    std::string stale_execution;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto pending = pending_skill_owners_.find(job.request_id);
      const bool current = pending != pending_skill_owners_.end() &&
                           pending->second.owner.token == job.owner.token &&
                           TokenCurrentLocked(job.owner);
      if (!current) {
        if (started) stale_execution = started->execution.execution_id;
      } else if (start_failed) {
        auto workflow = action_lists_.find(job.owner.workflow_id);
        for (auto& action : workflow->second.action_list["actions"])
          if (action.at("action_id") == job.owner.action_id &&
              !TerminalAction(action.at("status").get<std::string>())) {
            action["status"] = "rejected";
            action["reason"] = failure;
            workflow->second.action_list["revision"] =
                workflow->second.action_list["revision"].get<std::uint64_t>() + 1;
            workflow->second.action_list["updated_at_unix_ms"] = UnixMillis(Clock::now());
            Advance(workflow->second);
            PublishActionListLocked(workflow->second.action_list);
            break;
          }
      } else if (started) {
        execution_owners_[started->execution.execution_id] = job.owner;
        if (!pending->second.observed) ApplyExecutionLocked(job.owner, started->execution);
        eos::lib::LogEvent(Logger(), spdlog::level::debug, "action_list_target_dispatched",
                           {{"workflow_id", job.owner.workflow_id},
                            {"action_id", job.owner.action_id},
                            {"target_kind", "skill"},
                            {"target_name", job.skill_name},
                            {"target_version", job.skill_version},
                            {"execution_id", started->execution.execution_id}});
      }
      if (pending != pending_skill_owners_.end() && pending->second.owner.token == job.owner.token)
        pending_skill_owners_.erase(pending);
    }
    if (!stale_execution.empty()) CancelSkillNoexcept(stale_execution);
  }
}

bool Manager::RequestStopLocked(nlohmann::json& action) {
  const auto status = action.at("status").get<std::string>();
  if (status != "queued" && status != "running") return false;
  if (action.value("stop_requested", false)) return true;
  PendingCancellation cancellation;
  cancellation.primitive = action.contains("primitive");
  const char* identity = cancellation.primitive ? "command_id" : "execution_id";
  if (!action.contains(identity) || !action.at(identity).is_string() ||
      action.at(identity).get<std::string>().empty())
    return false;
  cancellation.target_id = action.at(identity).get<std::string>();
  action["stop_requested"] = true;
  pending_cancellations_.push_back(std::move(cancellation));
  return true;
}

void Manager::DrainDispatches() {
  while (true) {
    DrainSkillStarts();
    PendingCancellation cancellation;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_cancellations_.empty()) return;
      cancellation = std::move(pending_cancellations_.front());
      pending_cancellations_.pop_front();
    }
    if (cancellation.primitive)
      CancelPrimitive(cancellation.target_id);
    else
      CancelSkillNoexcept(cancellation.target_id);
  }
}

nlohmann::json Manager::Submit(const nlohmann::json& definition) {
  const std::string workflow_id = definition.is_object() && definition.contains("workflow_id") &&
                                          definition.at("workflow_id").is_string()
                                      ? definition.at("workflow_id").get<std::string>()
                                      : std::string();
  try {
    return SubmitImpl(definition);
  } catch (const std::invalid_argument&) {
    Log(spdlog::level::warn, "action_list_definition_rejected", workflow_id);
    throw;
  } catch (const std::logic_error&) {
    Log(spdlog::level::warn, "action_list_definition_rejected", workflow_id);
    throw;
  } catch (const std::exception&) {
    Log(spdlog::level::err, "action_list_boundary_failure", workflow_id);
    throw;
  }
}

nlohmann::json Manager::SubmitImpl(const nlohmann::json& definition) {
  if (!definition.is_object() || !definition.contains("workflow_id") ||
      !definition.at("workflow_id").is_string() || !definition.contains("actions") ||
      !definition.at("actions").is_array())
    throw std::invalid_argument("workflow requires workflow_id and actions");
  const auto id = definition.at("workflow_id").get<std::string>();
  if (id.empty()) throw std::invalid_argument("workflow_id must not be empty");
  if (definition.at("actions").empty()) throw std::invalid_argument("workflow must have actions");
  if (definition.at("actions").size() > max_actions_per_workflow_)
    throw std::length_error("workflow has too many actions");
  const std::string encoded_definition = definition.dump();
  if (encoded_definition.size() > max_workflow_bytes_)
    throw std::length_error("workflow definition exceeds configured size bound");
  std::vector<std::string> supersedes;
  if (definition.contains("supersedes_workflow_ids")) {
    if (!definition.at("supersedes_workflow_ids").is_array())
      throw std::invalid_argument("supersedes_workflow_ids must be an array");
    std::set<std::string> unique_supersedes;
    for (const auto& target : definition.at("supersedes_workflow_ids")) {
      if (!target.is_string() || target.get<std::string>().empty() ||
          !unique_supersedes.insert(target.get<std::string>()).second)
        throw std::invalid_argument("superseded workflow IDs must be unique non-empty strings");
      if (target.get<std::string>() == id)
        throw std::invalid_argument("a workflow cannot supersede itself");
      supersedes.push_back(target.get<std::string>());
    }
  }
  std::set<std::string> ids;
  std::map<std::string, std::size_t> action_positions;
  std::size_t action_position = 0;
  for (const auto& action : definition.at("actions")) {
    const unsigned target_count = static_cast<unsigned>(action.contains("skill")) +
                                  static_cast<unsigned>(action.contains("primitive")) +
                                  static_cast<unsigned>(action.contains("control")) +
                                  static_cast<unsigned>(action.contains("noop"));
    if (!action.is_object() || !action.contains("action_id") ||
        !action.at("action_id").is_string() || target_count != 1 ||
        !action.contains("parameters") || !action.at("parameters").is_object())
      throw std::invalid_argument(
          "each action requires action_id, one target, and object parameters");
    const auto action_id = action.at("action_id").get<std::string>();
    if (action_id.empty() || !ids.insert(action_id).second)
      throw std::invalid_argument("action IDs must be non-empty and unique");
    action_positions[action_id] = action_position++;
    if (action.contains("skill") || action.contains("primitive")) {
      const auto& target = action.contains("skill") ? action.at("skill") : action.at("primitive");
      if (!target.is_object() || !target.contains("name") || !target.at("name").is_string() ||
          target.at("name").get<std::string>().empty() || !target.contains("version") ||
          !target.at("version").is_string() || target.at("version").get<std::string>().empty())
        throw std::invalid_argument("action target requires name and version");
      if (action.contains("primitive") &&
          (!primitive_runner_.execute || !target.contains("device_id") ||
           !target.at("device_id").is_string() || action.at("parameters").contains("command_id")))
        throw std::invalid_argument(
            "primitive requires a runner, device_id string, and no parameters.command_id");
    } else if (action.contains("control")) {
      const auto& target = action.at("control");
      if (!target.is_object() || !target.contains("target_action_id") ||
          !target.at("target_action_id").is_string() ||
          target.at("target_action_id").get<std::string>().empty() ||
          target.value("operation", std::string()) != "terminate")
        throw std::invalid_argument("control target requires an action and terminate operation");
    } else if (!action.at("noop").is_object()) {
      throw std::invalid_argument("noop target must be an object");
    }
    const auto execution_mode = action.value("execution_mode", std::string());
    const auto monitor_lifetime = action.value("monitor_lifetime", std::string());
    if (action.contains("finalizer") && !action.at("finalizer").is_boolean())
      throw std::invalid_argument("finalizer must be a boolean");
    if (action.contains("continue_on_failure") &&
        !action.at("continue_on_failure").is_boolean())
      throw std::invalid_argument("continue_on_failure must be a boolean");
    if ((!execution_mode.empty() && execution_mode != "task" && execution_mode != "monitor") ||
        (!monitor_lifetime.empty() && monitor_lifetime != "explicit" &&
         monitor_lifetime != "workflow"))
      throw std::invalid_argument("invalid action execution mode or monitor lifetime");
    if ((!monitor_lifetime.empty() && execution_mode != "monitor") ||
        (execution_mode == "monitor" && monitor_lifetime.empty()) ||
        (execution_mode == "monitor" && (action.contains("control") || action.contains("noop"))))
      throw std::invalid_argument("monitor actions require a supported target and lifetime");
    if (action.contains("choice_group") &&
        (!action.at("choice_group").is_string() ||
         action.at("choice_group").get<std::string>().empty() || !action.contains("when")))
      throw std::invalid_argument("choice actions require a non-empty group and condition");
    if (action.contains("depends_on")) {
      if (!action.at("depends_on").is_array())
        throw std::invalid_argument("depends_on must be an array");
      std::set<std::string> unique_dependencies;
      for (const auto& dependency : action.at("depends_on"))
        if (!dependency.is_string() ||
            !unique_dependencies.insert(dependency.get<std::string>()).second)
          throw std::invalid_argument("dependency IDs must be unique strings");
    }
    if (action.contains("when")) {
      const auto& condition = action.at("when");
      if (!condition.is_object())
        throw std::invalid_argument("when must be an object");
      const bool resource_condition = condition.contains("resource");
      if (resource_condition) {
        if (!condition.at("resource").is_string() ||
            condition.at("resource").get<std::string>().empty() ||
            condition.contains("fact") || condition.contains("source_action_id") ||
            !condition.contains("equals") || !condition.contains("detection_kind") ||
            !condition.at("detection_kind").is_string() ||
            !condition.contains("detection_value_pointer") ||
            !condition.at("detection_value_pointer").is_string())
          throw std::invalid_argument(
              "resource condition requires resource, detection_kind, "
              "detection_value_pointer, and equals without fact fields");
        const std::string kind = condition.at("detection_kind").get<std::string>();
        if (kind != "human" && kind != "object")
          throw std::invalid_argument("resource condition detection_kind is invalid");
        if (!DetectionValuePointerAllowed(
                condition.at("detection_value_pointer").get<std::string>()))
          throw std::invalid_argument("resource condition detection_value_pointer is invalid");
      } else if (!condition.at("fact").is_string() ||
                 condition.at("fact").get<std::string>().empty() ||
                 !condition.contains("equals")) {
        throw std::invalid_argument("when requires fact and equals");
      }
    }
    if (action.contains("output_bindings") && !action.at("output_bindings").is_array())
      throw std::invalid_argument("output_bindings must be an array");
  }
  nlohmann::json admitted_actions = definition.at("actions");
  for (auto& action : admitted_actions) {
    std::set<std::string> dependencies;
    if (action.contains("depends_on"))
      for (const auto& dependency : action.at("depends_on"))
        if (!ids.count(dependency.get<std::string>()) ||
            dependency.get<std::string>() == action.at("action_id").get<std::string>())
          throw std::invalid_argument("unknown or self action dependency");
        else
          dependencies.insert(dependency.get<std::string>());
    if (action.contains("when") && action.at("when").contains("source_action_id")) {
      const auto& source = action.at("when").at("source_action_id");
      if (!source.is_string() || !ids.count(source.get<std::string>()) ||
          source.get<std::string>() == action.at("action_id").get<std::string>())
        throw std::invalid_argument("unknown or self condition source action");
      const auto& source_action =
          admitted_actions.at(action_positions.at(source.get<std::string>()));
      if (!source_action.contains("skill"))
        throw std::invalid_argument("condition source action must create a skill execution");
    }
    if (action.contains("control")) {
      const auto target = action.at("control").at("target_action_id").get<std::string>();
      if (!ids.count(target) || target == action.at("action_id").get<std::string>())
        throw std::invalid_argument("unknown or self control target action");
    }
    std::vector<std::vector<std::string>> destinations;
    for (const auto& binding : action.value("output_bindings", nlohmann::json::array())) {
      if (!binding.is_object() || !binding.contains("source_action_id") ||
          !binding.at("source_action_id").is_string() ||
          !binding.contains("source_output_pointer") ||
          !binding.at("source_output_pointer").is_string() ||
          !binding.contains("destination_parameter_pointer") ||
          !binding.at("destination_parameter_pointer").is_string())
        throw std::invalid_argument("each output binding requires source and pointer strings");
      const auto source = binding.at("source_action_id").get<std::string>();
      const auto action_id = action.at("action_id").get<std::string>();
      if (!action_positions.count(source))
        throw std::invalid_argument("output binding source action does not exist");
      if (action_positions.at(source) >= action_positions.at(action_id))
        throw std::invalid_argument("output binding source must precede its destination action");
      ParseJsonPointer(binding.at("source_output_pointer").get<std::string>());
      auto destination = ParseJsonPointer(
          binding.at("destination_parameter_pointer").get<std::string>());
      for (const auto& existing : destinations)
        if (PointerOverlaps(existing, destination))
          throw std::invalid_argument("output bindings have conflicting destinations");
      try {
        (void)action.at("parameters").at(nlohmann::json::json_pointer(
            binding.at("destination_parameter_pointer").get<std::string>()));
      } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("output binding destination does not exist in parameters");
      }
      destinations.push_back(std::move(destination));
      if (dependencies.insert(source).second) action["depends_on"].push_back(source);
    }
  }
  std::map<std::string, std::size_t> indegree;
  std::map<std::string, std::vector<std::string>> dependents;
  for (const auto& action : admitted_actions) indegree[action.at("action_id")] = 0;
  for (const auto& action : admitted_actions) {
    if (!action.contains("depends_on")) continue;
    for (const auto& dependency : action.at("depends_on")) {
      ++indegree[action.at("action_id").get<std::string>()];
      dependents[dependency.get<std::string>()].push_back(
          action.at("action_id").get<std::string>());
    }
  }
  std::vector<std::string> ready;
  for (const auto& [action_id, degree] : indegree)
    if (degree == 0) ready.push_back(action_id);
  std::size_t visited = 0;
  while (!ready.empty()) {
    const auto action_id = ready.back();
    ready.pop_back();
    ++visited;
    for (const auto& dependent : dependents[action_id])
      if (--indegree[dependent] == 0) ready.push_back(dependent);
  }
  if (visited != indegree.size())
    throw std::invalid_argument("workflow dependencies contain a cycle");

  Record workflow;
  const auto now = Clock::now();
  workflow.action_list = {{"workflow_id", id},
                          {"revision", 1},
                          {"status", "pending"},
                          {"created_at_unix_ms", UnixMillis(now)},
                          {"updated_at_unix_ms", UnixMillis(now)},
                          {"supersedes_workflow_ids", supersedes},
                          {"actions", nlohmann::json::array()}};
  for (const auto& spec : admitted_actions) {
    auto item = spec;
    if (!item.contains("depends_on")) item["depends_on"] = nlohmann::json::array();
    item["status"] = "blocked";
    workflow.action_list["actions"].push_back(std::move(item));
  }

  std::vector<std::string> executions_to_cancel;
  std::vector<std::string> commands_to_cancel;
  std::unique_lock<std::mutex> lock(mutex_);
  EnsureOperationalLocked();
  for (const auto& target : supersedes) {
    const auto old = action_lists_.find(target);
    if (old == action_lists_.end())
      throw std::invalid_argument("superseded ActionList does not exist: " + target);
    const auto old_status = old->second.action_list.at("status").get<std::string>();
    if (old_status != "pending" && old_status != "running" && old_status != "failed")
      throw std::logic_error("ActionList cannot be superseded in its current state: " + target);
  }
  const auto existing_list = action_lists_.find(id);
  if (!supersedes.empty() && existing_list != action_lists_.end())
    throw std::logic_error("a superseding ActionList must use a new workflow_id");
  if (existing_list != action_lists_.end()) {
    const auto& old_actions = existing_list->second.action_list.at("actions");
    const bool untouched =
        existing_list->second.action_list.at("status") == "pending" &&
        std::all_of(old_actions.begin(), old_actions.end(),
                    [](const auto& action) { return action.at("status") == "blocked"; });
    if (!untouched)
      throw std::logic_error("cannot replace an ActionList after an action has started");
  }
  const auto saved_action_lists = action_lists_;
  const auto saved_workflow_order = workflow_order_;
  const auto saved_pending_skill_starts = pending_skill_starts_;
  const auto saved_pending_skill_owners = pending_skill_owners_;
  const auto saved_pending_cancellations = pending_cancellations_;
  const auto saved_pending = pending_;
  const auto saved_primitive_cancellations = primitive_cancellations_;
  const auto saved_execution_owners = execution_owners_;
  const auto saved_skill_cancellations_in_flight = skill_cancellations_in_flight_;
  const auto saved_events = events_;
  const auto saved_next_event_sequence = next_event_sequence_;
  const auto saved_next_dispatch_token = next_dispatch_token_;
  journal_deferred_ = journal_ && !supersedes.empty();
  try {
    bool evicted = false;
    while (existing_list == action_lists_.end() && action_lists_.size() >= max_workflows_) {
      if (!EvictTerminalWorkflowLocked())
        throw std::length_error("workflow retention limit reached with all workflows active");
      evicted = true;
    }
    if (evicted) CompactJournalLocked();
    for (const auto& target : supersedes) {
      auto& old_record = action_lists_.at(target);
      auto& old = old_record.action_list;
      old["status"] = "superseding";
      old["superseded_by_workflow_id"] = id;
      for (auto& action : old["actions"]) {
        const auto status = action.at("status").get<std::string>();
        if (status == "blocked" || status == "ready") {
          if (action.value("finalizer", false)) continue;
          action["status"] = "cancelled";
          action["reason"] = "superseded:" + id;
          old_record.action_tokens.erase(action.at("action_id").get<std::string>());
        } else if (status == "queued" || status == "running") {
          if (action.contains("primitive"))
            commands_to_cancel.push_back(action.at("command_id").get<std::string>());
          else
            executions_to_cancel.push_back(action.at("execution_id").get<std::string>());
          action["reason"] = "superseded:" + id;
        }
      }
      for (auto pending = pending_skill_owners_.begin(); pending != pending_skill_owners_.end();) {
        if (pending->second.owner.workflow_id == target)
          pending = pending_skill_owners_.erase(pending);
        else
          ++pending;
      }
      old["revision"] = old["revision"].get<std::uint64_t>() + 1;
      old["updated_at_unix_ms"] = UnixMillis(Clock::now());
      Advance(action_lists_.at(target));
      PublishActionListLocked(old);
      eos::lib::LogEvent(Logger(), spdlog::level::info, "action_list_supersession_requested",
                         {{"workflow_id", target}, {"superseded_by_workflow_id", id}});
    }
    auto found = action_lists_.find(id);
    if (found != action_lists_.end()) {
      workflow.action_list["revision"] =
          found->second.action_list.at("revision").get<std::uint64_t>() + 1;
      workflow.action_list["created_at_unix_ms"] =
          found->second.action_list.at("created_at_unix_ms");
      found->second = std::move(workflow);
      Log(spdlog::level::info, "action_list_replaced", id, {},
          found->second.action_list.at("status"), found->second.action_list.at("revision"));
    } else {
      action_lists_.emplace(id, std::move(workflow));
      workflow_order_.push_back(id);
    }
    auto& created = action_lists_.at(id);
    created.dispatch_deferred = !supersedes.empty();
    if (!created.dispatch_deferred) Advance(created);
    PublishActionListLocked(created.action_list);
    if (journal_deferred_) CompactJournalLocked();
    journal_deferred_ = false;
  } catch (...) {
    journal_deferred_ = false;
    action_lists_ = saved_action_lists;
    workflow_order_ = saved_workflow_order;
    if (!journal_failed_) {
      pending_skill_starts_ = saved_pending_skill_starts;
      pending_skill_owners_ = saved_pending_skill_owners;
      pending_cancellations_ = saved_pending_cancellations;
      pending_ = saved_pending;
      primitive_cancellations_ = saved_primitive_cancellations;
    }
    execution_owners_ = saved_execution_owners;
    skill_cancellations_in_flight_ = saved_skill_cancellations_in_flight;
    events_ = saved_events;
    next_event_sequence_ = saved_next_event_sequence;
    next_dispatch_token_ = saved_next_dispatch_token;
    throw;
  }
  Log(spdlog::level::info, "action_list_admitted", id, {},
      action_lists_.at(id).action_list.at("status"),
      action_lists_.at(id).action_list.at("revision"));
  lock.unlock();
  for (const auto& execution : executions_to_cancel) CancelSkillNoexcept(execution);
  for (const auto& command : commands_to_cancel) CancelPrimitive(command);
  if (!supersedes.empty()) {
    std::lock_guard<std::mutex> dispatch_lock(mutex_);
    auto& accepted = action_lists_.at(id);
    accepted.dispatch_deferred = false;
    Advance(accepted);
    PublishActionListLocked(accepted.action_list);
  }
  DrainDispatches();
  return Get(id).value();
}

std::optional<nlohmann::json> Manager::Get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureOperationalLocked();
  auto it = action_lists_.find(id);
  return it == action_lists_.end() ? std::nullopt
                                   : std::optional<nlohmann::json>(it->second.action_list);
}

std::vector<nlohmann::json> Manager::List() const {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureOperationalLocked();
  std::vector<nlohmann::json> result;
  result.reserve(action_lists_.size());
  for (const auto& [_, workflow] : action_lists_) result.push_back(workflow.action_list);
  return result;
}

RuntimeSnapshot Manager::Snapshot() const {
  RuntimeSnapshot snapshot;
  snapshot.executions = skill_runner_.list_executions();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOperationalLocked();
    snapshot.facts = facts_;
    for (const auto& [_, action_list] : action_lists_)
      snapshot.action_lists.push_back(action_list.action_list);
    snapshot.cursor = {event_epoch_, next_event_sequence_ - 1};
  }
  snapshot.captured_at = Clock::now();
  return snapshot;
}

bool Manager::UpdateFact(const std::string& key, StateFact fact) {
  if (key.empty() || fact.source.empty() || fact.sequence == 0)
    throw std::invalid_argument("fact key, source, and non-zero sequence are required");
  if (fact.received_time == Clock::time_point{}) fact.received_time = Clock::now();
  if (fact.event_time == Clock::time_point{}) fact.event_time = fact.received_time;
  if (fact.event_time > fact.received_time + std::chrono::minutes(5))
    throw std::invalid_argument("fact event timestamp is too far in the future");
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOperationalLocked();
    const auto source_sequence = fact_source_sequences_[key].find(fact.source);
    if (source_sequence != fact_source_sequences_[key].end() &&
        fact.sequence <= source_sequence->second)
      return false;
    auto it = facts_.find(key);
    if (it != facts_.end()) {
      const auto& previous = it->second;
      if ((fact.source == previous.source && fact.event_time < previous.event_time) ||
          fact.received_time < previous.received_time ||
          (fact.received_time == previous.received_time && fact.source < previous.source)) {
        eos::lib::LogEvent(
            Logger(), spdlog::level::debug, "action_list_fact_rejected",
            {{"fact_key", key}, {"source", fact.source}, {"sequence", fact.sequence}});
        return false;
      }
    }
    fact_source_sequences_[key][fact.source] = fact.sequence;
    facts_[key] = std::move(fact);
    eos::lib::LogEvent(Logger(), spdlog::level::debug, "action_list_fact_updated",
                       {{"fact_key", key},
                        {"source", facts_.at(key).source},
                        {"sequence", facts_.at(key).sequence}});
    PublishFactLocked(key, facts_.at(key));
    for (auto& [_, workflow] : action_lists_) {
      const auto revision = workflow.action_list.at("revision").get<std::uint64_t>();
      Advance(workflow);
      if (workflow.action_list.at("revision").get<std::uint64_t>() != revision)
        PublishActionListLocked(workflow.action_list);
    }
  }
  DrainDispatches();
  return true;
}

void Manager::NotifyExternalState() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOperationalLocked();
    for (auto& [_, workflow] : action_lists_) {
      const auto revision = workflow.action_list.at("revision").get<std::uint64_t>();
      Advance(workflow);
      if (workflow.action_list.at("revision").get<std::uint64_t>() != revision)
        PublishActionListLocked(workflow.action_list);
    }
  }
  DrainDispatches();
}

bool Manager::ResourceConditionMatches(
    const nlohmann::json& condition, Clock::time_point* matched_observed_at) const {
  if (!read_resource_) return false;
  const auto snapshot = read_resource_(condition.at("resource").get<std::string>());
  if (!snapshot) return false;
  if (condition.contains("max_age_ms") &&
      Clock::now() - snapshot->observed_at >
          std::chrono::milliseconds(condition.at("max_age_ms").get<std::int64_t>()))
    return false;
  const std::string kind = condition.at("detection_kind").get<std::string>();
  const nlohmann::json::json_pointer pointer(
      condition.at("detection_value_pointer").get<std::string>());
  const auto detections = snapshot->value.find("detections");
  if (detections == snapshot->value.end() || !detections->is_array()) return false;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != kind) continue;
    try {
      if (detection.at(pointer) == condition.at("equals")) {
        if (matched_observed_at != nullptr) *matched_observed_at = snapshot->observed_at;
        return true;
      }
    } catch (const nlohmann::json::out_of_range&) {
    }
  }
  return false;
}

bool Manager::ConditionMatches(const nlohmann::json& list, const nlohmann::json& action,
                               Clock::time_point* matched_observed_at) const {
  if (!action.contains("when")) return true;
  const auto& condition = action.at("when");
  if (condition.contains("resource"))
    return ResourceConditionMatches(condition, matched_observed_at);
  const auto fact = facts_.find(condition.at("fact").get<std::string>());
  if (fact == facts_.end() || fact->second.value != condition.at("equals")) return false;
  if (condition.contains("source_action_id")) {
    const auto& source = FindAction(list, condition.at("source_action_id").get<std::string>());
    if (!source.contains("execution_id") || source.at("execution_id") != fact->second.source)
      return false;
  }
  if (condition.contains("max_age_ms") &&
      Clock::now() - fact->second.event_time >
          std::chrono::milliseconds(condition.at("max_age_ms").get<std::int64_t>()))
    return false;
  return true;
}

void Manager::ObserveExecution(const skills::ExecutionSnapshot& execution) {
  bool owned = false;
  bool start_in_flight = false;
  bool cancellation_in_flight = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOperationalLocked();
    cancellation_in_flight = skill_cancellations_in_flight_.count(execution.execution_id) != 0;
    SkillOwner action_owner;
    const auto existing = execution_owners_.find(execution.execution_id);
    if (existing != execution_owners_.end()) {
      action_owner = existing->second;
      owned = true;
    }
    const auto pending = pending_skill_owners_.find(execution.request_id);
    if (pending != pending_skill_owners_.end() && TokenCurrentLocked(pending->second.owner)) {
      start_in_flight = true;
      pending->second.observed = true;
      if (!owned) {
        action_owner = pending->second.owner;
        execution_owners_[execution.execution_id] = action_owner;
        owned = true;
      }
    }
    if (owned) ApplyExecutionLocked(action_owner, execution);
  }
  if (owned && !start_in_flight && !cancellation_in_flight) DrainDispatches();
}

void Manager::ObserveSignal(const skills::ExecutionSnapshot& execution,
                            const skills::Signal& signal) {
  if (signal.name != "state.fact" || !signal.payload.contains("key") ||
      !signal.payload.at("key").is_string() ||
      signal.payload.at("key").get<std::string>().empty() || !signal.payload.contains("value") ||
      !signal.payload.contains("sequence") || !signal.payload.at("sequence").is_number_integer() ||
      signal.payload.at("sequence").get<std::int64_t>() <= 0)
    return;
  StateFact fact;
  fact.value = signal.payload.at("value");
  fact.source = execution.execution_id;
  fact.sequence = signal.payload.at("sequence").get<std::uint64_t>();
  if (signal.payload.contains("event_time_unix_ms")) {
    if (!signal.payload.at("event_time_unix_ms").is_number_integer()) return;
    fact.event_time = Clock::time_point(
        std::chrono::milliseconds(signal.payload.at("event_time_unix_ms").get<std::int64_t>()));
  }
  UpdateFact(signal.payload.at("key").get<std::string>(), std::move(fact));
}

void Manager::Refresh(const std::string& id) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    EnsureOperationalLocked();
  }
  const auto executions = skill_runner_.list_executions();
  std::set<std::string> owned;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [execution, owner] : execution_owners_)
      if (owner.workflow_id == id) owned.insert(execution);
  }
  for (const auto& execution : executions)
    if (owned.count(execution.execution_id)) ObserveExecution(execution);
}

void Manager::Advance(Record& record) {
  if (record.dispatch_deferred) return;
  auto& list = record.action_list;
  const auto list_status = list.at("status").get<std::string>();
  if (list_status == "superseded") {
    UpdateStatus(record);
    return;
  }
  bool failed = false;
  for (const auto& action : list["actions"])
    failed = failed || (!action.value("continue_on_failure", false) &&
                        FailedAction(action.at("status").get<std::string>()));
  auto stop_workflow_monitors = [&](bool force) {
    bool others_done = true;
    for (const auto& action : list["actions"])
      if (action.value("monitor_lifetime", std::string()) != "workflow")
        others_done =
            others_done &&
            (SuccessfulAction(action.at("status").get<std::string>()) ||
             (action.value("continue_on_failure", false) &&
              FailedAction(action.at("status").get<std::string>())));
    if (!force && !others_done) return false;
    bool requested = false;
    for (auto& action : list["actions"]) {
      const bool selected_monitor =
          force ? action.value("execution_mode", std::string()) == "monitor"
                : action.value("monitor_lifetime", std::string()) == "workflow";
      if (selected_monitor && !action.value("stop_requested", false))
        requested = RequestStopLocked(action) || requested;
    }
    return requested;
  };
  if (failed) {
    bool abandoned = false;
    for (auto& action : list["actions"]) {
      if (action.value("finalizer", false)) continue;
      const std::string status = action.at("status").get<std::string>();
      if (status != "blocked" && status != "ready") continue;
      action["status"] = "skipped";
      action["reason"] = "workflow_failed";
      abandoned = true;
    }
    if (stop_workflow_monitors(true)) {
      list["revision"] = list["revision"].get<std::uint64_t>() + 1;
      list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    }
    if (abandoned) {
      list["revision"] = list["revision"].get<std::uint64_t>() + 1;
      list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    }
  }
  bool changed = false;
  for (auto& action : list["actions"]) {
    const auto status = action.at("status").get<std::string>();
    if (status != "blocked" && status != "ready") continue;
    if (failed && !action.value("finalizer", false)) continue;
    bool unavailable_output = false;
    std::string unavailable_source;
    for (const auto& binding : action.value("output_bindings", nlohmann::json::array())) {
      const auto source_id = binding.at("source_action_id").get<std::string>();
      const auto& source = FindAction(list, source_id);
      if (FailedAction(source.at("status").get<std::string>())) {
        unavailable_output = true;
        unavailable_source = source_id;
        break;
      }
    }
    if (unavailable_output) {
      action["status"] = "skipped";
      action["reason"] = "dependency_output_unavailable:" + unavailable_source;
      changed = true;
      continue;
    }
    if (action.contains("when")) {
      std::string failed_dependency;
      for (const auto& dependency_id : action.at("depends_on")) {
        const auto& dependency = FindAction(list, dependency_id.get<std::string>());
        if (dependency.value("continue_on_failure", false) &&
            FailedAction(dependency.at("status").get<std::string>())) {
          failed_dependency = dependency_id.get<std::string>();
          break;
        }
      }
      if (!failed_dependency.empty()) {
        action["status"] = "skipped";
        action["reason"] = "dependency_failed_tolerated:" + failed_dependency;
        changed = true;
        continue;
      }
    }
    bool dependencies_done = true;
    for (const auto& dependency : action.at("depends_on"))
      dependencies_done =
          dependencies_done && DependencySatisfied(list, action, dependency.get<std::string>());
    if (!dependencies_done) continue;
    Clock::time_point matched_observed_at{};
    const bool condition_matches = ConditionMatches(list, action, &matched_observed_at);
    if (action.contains("choice_group") && action.contains("when") &&
        action.at("when").contains("resource")) {
      const std::string pointer =
          action.at("when").value("detection_value_pointer", std::string());
      const bool stabilize_posture =
          pointer == "/attributes/pose" || pointer == "/attributes/posture_state";
      const std::string action_id = action.at("action_id").get<std::string>();
      if (!condition_matches) {
        record.resource_condition_first_match.erase(action_id);
        continue;
      }
      if (stabilize_posture) {
        auto [first, inserted] =
            record.resource_condition_first_match.emplace(action_id, matched_observed_at);
        if (inserted || matched_observed_at < first->second ||
            matched_observed_at - first->second < std::chrono::milliseconds(1500))
          continue;
      }
    } else if (!condition_matches) {
      continue;
    }
    if (action.contains("choice_group")) {
      const auto group = action.at("choice_group").get<std::string>();
      const auto selected = action.at("action_id").get<std::string>();
      for (auto& alternative : list["actions"]) {
        if (alternative.at("action_id") == selected ||
            alternative.value("choice_group", std::string()) != group)
          continue;
        const auto alternative_status = alternative.at("status").get<std::string>();
        if (alternative_status == "blocked" || alternative_status == "ready") {
          alternative["status"] = "skipped";
          alternative["reason"] = "choice_not_selected:" + selected;
          changed = true;
        }
      }
    }
    action["status"] = "ready";
    try {
      if (!action.contains("resolved_parameters"))
        action["resolved_parameters"] = ResolveParameters(list, action);
      if (action.contains("noop")) {
        action["status"] = "succeeded";
        action["result"] = nlohmann::json::object();
        action["output"] = nullptr;
      } else if (action.contains("control")) {
        nlohmann::json* target = nullptr;
        const auto target_id = action.at("control").at("target_action_id").get<std::string>();
        for (auto& candidate : list["actions"])
          if (candidate.at("action_id") == target_id) {
            target = &candidate;
            break;
          }
        if (target == nullptr) throw std::logic_error("control target action is missing");
        const auto target_status = target->at("status").get<std::string>();
        if (!TerminalAction(target_status) && !RequestStopLocked(*target))
          throw std::logic_error("control target action is not active");
        action["status"] = "succeeded";
        action["result"] = nlohmann::json::object();
        action["output"] = nullptr;
      } else if (action.contains("primitive")) {
        PrimitiveJob job;
        job.workflow_id = list.at("workflow_id").get<std::string>();
        job.action_id = action.at("action_id").get<std::string>();
        job.command_id = lib::MintUuidV7();
        job.primitive_name = action.at("primitive").at("name").get<std::string>();
        job.primitive_version = action.at("primitive").at("version").get<std::string>();
        job.device_id = action.at("primitive").at("device_id").get<std::string>();
        job.arguments = action.at("resolved_parameters");
        job.correlation_id = eos::lib::CurrentCorrelationId();
        job.cancel_requested = std::make_shared<std::atomic_bool>(false);
        action["command_id"] = job.command_id;
        if (stopping_) throw std::runtime_error("ActionList manager is stopping");
        primitive_cancellations_[job.command_id] = job.cancel_requested;
        pending_.push_back(std::move(job));
        action["status"] = "queued";
        eos::lib::LogEvent(Logger(), spdlog::level::debug, "action_list_target_dispatched",
                           {{"workflow_id", list.at("workflow_id")},
                            {"action_id", action.at("action_id")},
                            {"target_kind", "primitive"},
                            {"target_name", action.at("primitive").at("name")},
                            {"target_version", action.at("primitive").at("version")}});
      } else {
        SkillStartJob job;
        job.owner.workflow_id = list.at("workflow_id").get<std::string>();
        job.owner.action_id = action.at("action_id").get<std::string>();
        job.owner.token = next_dispatch_token_++;
        job.request_id = "actionlist:" + lib::MintUuidV7();
        job.skill_name = action.at("skill").at("name").get<std::string>();
        job.skill_version = action.at("skill").at("version").get<std::string>();
        job.arguments = action.at("resolved_parameters");
        record.action_tokens[job.owner.action_id] = job.owner.token;
        pending_skill_owners_[job.request_id] = {job.owner, false};
        pending_skill_starts_.push_back(std::move(job));
      }
    } catch (const std::exception& error) {
      action["status"] = "rejected";
      action["reason"] = error.what();
    }
    changed = true;
  }
  changed = stop_workflow_monitors(false) || changed;
  if (changed) {
    list["revision"] = list["revision"].get<std::uint64_t>() + 1;
    list["updated_at_unix_ms"] = UnixMillis(Clock::now());
  }
  UpdateStatus(record);
}

void Manager::UpdateStatus(Record& record) {
  auto& list = record.action_list;
  if (list.at("status") == "superseding") {
    const bool active =
        std::any_of(list["actions"].begin(), list["actions"].end(), [](const auto& action) {
          const auto status = action.at("status").template get<std::string>();
          return status == "queued" || status == "running";
        });
    const bool finalizers_complete =
        std::all_of(list["actions"].begin(), list["actions"].end(), [](const auto& action) {
          return !action.value("finalizer", false) ||
                 TerminalAction(action.at("status").template get<std::string>());
        });
    if (!active && finalizers_complete) {
      list["status"] = "superseded";
      list["revision"] = list["revision"].get<std::uint64_t>() + 1;
      list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    }
    return;
  }
  if (list.at("status") == "superseded") return;
  bool all_succeeded = !list["actions"].empty();
  bool any_started = false;
  bool any_failed = false;
  bool finalizers_complete = true;
  for (const auto& action : list["actions"]) {
    const auto status = action.at("status").get<std::string>();
    all_succeeded =
        all_succeeded &&
        (SuccessfulAction(status) ||
         (action.value("continue_on_failure", false) && FailedAction(status)));
    any_started =
        any_started || status == "queued" || status == "running" || TerminalAction(status);
    any_failed = any_failed ||
                 (!action.value("continue_on_failure", false) && FailedAction(status));
    if (action.value("finalizer", false))
      finalizers_complete = finalizers_complete && TerminalAction(status);
  }
  const std::string next =
      any_failed && finalizers_complete
          ? "failed"
          : (all_succeeded ? "succeeded" : (any_started ? "running" : "pending"));
  if (list["status"] != next) {
    list["status"] = next;
    list["revision"] = list["revision"].get<std::uint64_t>() + 1;
    list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    std::string failed_action_id;
    if (next == "failed") {
      for (const auto& action : list["actions"]) {
        const std::string action_status = action.at("status").get<std::string>();
        if (action_status == "failed" || action_status == "rejected" ||
            action_status == "cancelled" || action_status == "timed_out" ||
            action_status == "outcome_unknown") {
          failed_action_id = action.value("action_id", "");
          break;
        }
      }
    }
    Log(next == "failed" ? spdlog::level::warn : spdlog::level::debug, "action_list_status_changed",
        list.at("workflow_id"), failed_action_id, next,
        list.at("revision").get<std::uint64_t>());
  }
}

void Manager::PublishActionListLocked(const nlohmann::json& action_list) {
  AppendJournalLocked("action_list", action_list);
  Event event;
  event.cursor = {event_epoch_, next_event_sequence_++};
  event.observed_at = Clock::now();
  event.kind = Event::Kind::kActionList;
  event.action_list = action_list;
  events_.push_back(std::move(event));
  while (events_.size() > max_events_) events_.pop_front();
  work_available_.notify_all();
  events_changed_.notify_all();
}

void Manager::PublishFactLocked(const std::string& key, const StateFact& fact) {
  AppendJournalLocked("fact", {{"key", key},
                               {"value", fact.value},
                               {"source", fact.source},
                               {"sequence", fact.sequence},
                               {"event_time_unix_ms", UnixMillis(fact.event_time)},
                               {"received_time_unix_ms", UnixMillis(fact.received_time)}});
  Event event;
  event.cursor = {event_epoch_, next_event_sequence_++};
  event.observed_at = Clock::now();
  event.kind = Event::Kind::kFact;
  event.fact_key = key;
  event.fact = fact;
  events_.push_back(std::move(event));
  while (events_.size() > max_events_) events_.pop_front();
  events_changed_.notify_all();
}

void Manager::AppendJournalLocked(const std::string& kind, const nlohmann::json& payload) {
  if (!journal_ || journal_deferred_) return;
  if (journal_failed_) throw std::runtime_error("ActionList journal is unavailable");
  try {
    journal_->repository.Append(kind, payload);
  } catch (const std::length_error&) {
    try {
      CompactJournalLocked();
      journal_->repository.Append(kind, payload);
      return;
    } catch (...) {
      EnterJournalFailStopLocked();
      throw;
    }
  } catch (...) {
    EnterJournalFailStopLocked();
    throw;
  }
}

void Manager::EnsureOperationalLocked() const {
  if (journal_failed_)
    throw std::runtime_error("ActionList manager stopped after a journal failure");
}

void Manager::EnterJournalFailStopLocked() noexcept {
  journal_failed_ = true;
  stopping_ = true;
  for (const auto& job : pending_) primitive_cancellations_.erase(job.command_id);
  pending_.clear();
  pending_skill_starts_.clear();
  pending_skill_owners_.clear();
  pending_cancellations_.clear();
  work_available_.notify_all();
  events_changed_.notify_all();
}

bool Manager::EvictTerminalWorkflowLocked() {
  for (auto order = workflow_order_.begin(); order != workflow_order_.end(); ++order) {
    const auto workflow = action_lists_.find(*order);
    if (workflow == action_lists_.end()) continue;
    const std::string status = workflow->second.action_list.value("status", std::string());
    if (status != "succeeded" && status != "failed" && status != "superseded") continue;
    const std::string id = *order;
    action_lists_.erase(workflow);
    workflow_order_.erase(order);
    for (auto owner = execution_owners_.begin(); owner != execution_owners_.end();) {
      if (owner->second.workflow_id == id)
        owner = execution_owners_.erase(owner);
      else
        ++owner;
    }
    return true;
  }
  return false;
}

void Manager::CompactJournalLocked() {
  if (!journal_) return;
  std::vector<nlohmann::json> records;
  records.reserve(action_lists_.size() + facts_.size());
  for (const auto& [_, workflow] : action_lists_)
    records.push_back({{"kind", "action_list"}, {"payload", workflow.action_list}});
  for (const auto& [key, fact] : facts_)
    records.push_back({{"kind", "fact"},
                       {"payload", {{"key", key},
                                    {"value", fact.value},
                                    {"source", fact.source},
                                    {"sequence", fact.sequence},
                                    {"event_time_unix_ms", UnixMillis(fact.event_time)},
                                    {"received_time_unix_ms", UnixMillis(fact.received_time)}}}});
  try {
    journal_->repository.Rewrite(records);
  } catch (...) {
    EnterJournalFailStopLocked();
    throw;
  }
}

void Manager::RecoverJournal() {
  for (const auto& entry : journal_->repository.Replay()) {
    const std::string kind = entry.at("kind").get<std::string>();
    const auto& payload = entry.at("payload");
    if (kind == "action_list") {
      if (!payload.is_object() || !payload.contains("workflow_id") ||
          !payload.at("workflow_id").is_string() ||
          payload.at("workflow_id").get<std::string>().empty() || !payload.contains("actions") ||
          !payload.at("actions").is_array())
        throw std::runtime_error("invalid ActionList snapshot in workflow journal");
      action_lists_[payload.at("workflow_id").get<std::string>()].action_list = payload;
    } else if (kind == "fact") {
      if (!payload.is_object() || !payload.contains("key") || !payload.at("key").is_string() ||
          payload.at("key").get<std::string>().empty() || !payload.contains("source") ||
          !payload.at("source").is_string() || !payload.contains("sequence") ||
          !payload.at("sequence").is_number_unsigned() ||
          !payload.contains("event_time_unix_ms") ||
          !payload.at("event_time_unix_ms").is_number_integer() ||
          !payload.contains("received_time_unix_ms") ||
          !payload.at("received_time_unix_ms").is_number_integer())
        throw std::runtime_error("invalid fact snapshot in workflow journal");
      facts_[payload.at("key").get<std::string>()] = {
          payload.value("value", nlohmann::json(nullptr)),
          payload.at("source").get<std::string>(),
          payload.at("sequence").get<std::uint64_t>(),
          Clock::time_point(std::chrono::milliseconds(
              payload.at("event_time_unix_ms").get<std::int64_t>())),
          Clock::time_point(std::chrono::milliseconds(
              payload.at("received_time_unix_ms").get<std::int64_t>()))};
      fact_source_sequences_[payload.at("key").get<std::string>()]
                            [payload.at("source").get<std::string>()] =
          payload.at("sequence").get<std::uint64_t>();
    } else {
      throw std::runtime_error("unknown workflow journal record kind: " + kind);
    }
  }

  std::vector<std::pair<std::int64_t, std::string>> recovered_order;
  recovered_order.reserve(action_lists_.size());
  for (const auto& [id, record] : action_lists_)
    recovered_order.emplace_back(record.action_list.value("created_at_unix_ms", 0LL), id);
  std::sort(recovered_order.begin(), recovered_order.end());
  for (const auto& [_, id] : recovered_order) workflow_order_.push_back(id);

  for (auto& [workflow_id, record] : action_lists_) {
    const nlohmann::json before = record.action_list;
    for (auto& action : record.action_list.at("actions")) {
      const std::string status = action.value("status", std::string());
      if (status != "ready" && status != "queued" && status != "running") continue;
      action["status"] = "outcome_unknown";
      action["reason"] = "runtime restarted before a terminal result was recorded";
      action["result"] = nlohmann::json::object();
      action["output"] = nlohmann::json(nullptr);
    }
    if (record.action_list != before) {
      record.action_list["revision"] = record.action_list.value("revision", 0U) + 1;
      record.action_list["updated_at_unix_ms"] = UnixMillis(Clock::now());
    }
    Advance(record);
    if (record.action_list != before) {
      journal_->repository.Append("action_list", record.action_list);
      Log(spdlog::level::warn, "action_list_recovered_ambiguous", workflow_id, {},
          record.action_list.value("status", std::string()),
          record.action_list.value("revision", 0U));
    }
  }
}

EventBatch Manager::WaitForEvents(const EventCursor& after, std::chrono::milliseconds wait) const {
  std::unique_lock<std::mutex> lock(mutex_);
  EnsureOperationalLocked();
  if (after.epoch != event_epoch_) return {ReplayStatus::kEpochMismatch, {}};
  if (after.sequence > next_event_sequence_ - 1) return {ReplayStatus::kFuture, {}};
  auto expired = [&] {
    return !events_.empty() && after.sequence < events_.front().cursor.sequence - 1;
  };
  if (expired()) return {ReplayStatus::kExpired, {}};
  events_changed_.wait_for(lock, wait, [&] {
    return stopping_ || expired() || next_event_sequence_ - 1 > after.sequence;
  });
  if (expired()) return {ReplayStatus::kExpired, {}};
  EventBatch batch;
  for (const auto& event : events_)
    if (event.cursor.sequence > after.sequence) batch.events.push_back(event);
  return batch;
}

}  // namespace eos::runtime::actionlist

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <regex>
#include <stdexcept>
#include <utility>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/skills/manager/logging.h"
#include "src/eos/runtime/skills/native/detail.h"
#include "src/eos/runtime/skills/core/admission.h"
#include "src/eos/runtime/skills/native/primitive_ack.h"

namespace eos::runtime::skills::native {
namespace detail {
PrimitiveAcknowledgement ParsePrimitiveAcknowledgement(const nlohmann::json& acknowledgement) {
  if (!acknowledgement.is_object())
    return {PrimitiveTerminalStatus::kInvalid,
            "primitive dispatcher returned a non-object acknowledgement"};

  const auto accepted = acknowledgement.find("accepted");
  if (accepted == acknowledgement.end() || !accepted->is_boolean())
    return {PrimitiveTerminalStatus::kInvalid,
            "primitive acknowledgement has no boolean accepted field"};

  const auto result = acknowledgement.find("result");
  if (result == acknowledgement.end() || !result->is_object())
    return {PrimitiveTerminalStatus::kInvalid,
            "primitive acknowledgement has no terminal result object"};
  const auto status = result->find("status");
  if (status == result->end() || !status->is_string())
    return {PrimitiveTerminalStatus::kInvalid,
            "primitive acknowledgement has no terminal result status"};

  const std::string value = status->get<std::string>();
  const bool was_accepted = accepted->get<bool>();
  if (was_accepted && value == "succeeded") return {PrimitiveTerminalStatus::kSucceeded, {}};
  if (was_accepted && value == "failed") {
    const auto reason = acknowledgement.find("reason");
    return {PrimitiveTerminalStatus::kFailed, reason != acknowledgement.end() && reason->is_string()
                                                  ? reason->get<std::string>()
                                                  : "primitive execution failed"};
  }
  if (!was_accepted && value == "rejected") return {PrimitiveTerminalStatus::kRejected, {}};
  if (value == "cancelled") return {PrimitiveTerminalStatus::kCancelled, {}};
  if (was_accepted && value == "timed_out") return {PrimitiveTerminalStatus::kTimedOut, {}};
  if (was_accepted && value == "outcome_unknown")
    return {PrimitiveTerminalStatus::kOutcomeUnknown, {}};

  return {PrimitiveTerminalStatus::kInvalid,
          "primitive acknowledgement has inconsistent accepted and result status fields"};
}
}  // namespace detail

namespace {
using namespace detail;

struct DeviceLease {
  std::function<void()> release;
  ~DeviceLease() {
    if (release) release();
  }
};

struct AsyncPrimitive {
  std::uint64_t id{0};
  std::string command_id;
  std::string primitive;
  std::string version;
  std::string device_id;
  std::string activity_id;
  std::atomic<bool> cancelled{false};
  DeviceLease lease;
  std::future<dispatch::DispatchResult> result;
};

struct ExecutionState {
  const LoadedSkill* skill{nullptr};
  dispatch::Dispatcher* dispatcher{nullptr};
  PrimitiveExecutor* primitive_executor{nullptr};
  Cancelled cancelled;
  ExecutionHooks hooks;
  std::string command_id;
  std::size_t call_index{0};
  bool release_latched{false};
  bool cancellation_latched{false};
  std::string last_failure_reason;
  std::string last_failure_message;
  std::size_t failure_reports{0};
  std::string reported_failure_reason;
  std::string reported_failure_message;
  std::string failure_report_error;
  ResourceReader resource_reader;
  InferenceRunner inference_runner;
  std::string acknowledgement;
  std::string error;
  std::string resource_value;
  std::string resource_error;
  std::string signal_name;
  std::string signal_payload;
  std::string source_topic;
  std::string source_value;
  std::string source_error;
  std::string inference_result;
  std::string inference_error;
  std::vector<nlohmann::json> acknowledgements;
  double log_tokens{64.0};
  std::chrono::steady_clock::time_point log_refill{std::chrono::steady_clock::now()};
  std::size_t logs_accepted{0};
  std::size_t logs_dropped{0};
  std::map<std::uint64_t, std::unique_ptr<AsyncPrimitive>> async_primitives;
};

class ActivityScope {
 public:
  ActivityScope(ExecutionState& state, std::string name,
                nlohmann::json fields = nlohmann::json::object())
      : state_(state) {
    if (state_.hooks.begin_activity)
      id_ = state_.hooks.begin_activity({std::move(name), std::move(fields)});
  }
  ~ActivityScope() {
    if (!id_.empty() && state_.hooks.end_activity) state_.hooks.end_activity(id_, outcome_);
  }
  void Outcome(std::string value) { outcome_ = std::move(value); }

 private:
  ExecutionState& state_;
  std::string id_;
  std::string outcome_{"failed"};
};

bool CheckCancelled(ExecutionState* state) {
  if (state == nullptr || !state->cancelled) return false;
  try {
    if (!state->cancelled()) return false;
  } catch (...) {
    LogSkillEvent(spdlog::level::err, "skill_cancel_check_failed",
                  {{"reason", "callback_exception"}});
  }
  state->release_latched = true;
  state->cancellation_latched = true;
  return true;
}

struct CompletionState {
  std::size_t calls{0};
  bool valid{false};
  bool accepted{false};
  std::string reason_code;
  std::string message;
  nlohmann::json result{nlohmann::json::object()};
  std::string error;
};

void RememberFailure(ExecutionState& state, std::string reason, std::string message) {
  state.last_failure_reason = std::move(reason);
  state.last_failure_message = std::move(message);
}

const char* RunStatusName(RunResult::Status status) {
  switch (status) {
    case RunResult::Status::kCompleted:
      return "completed";
    case RunResult::Status::kCancelled:
      return "cancelled";
    case RunResult::Status::kInvalidArgument:
      return "invalid_argument";
    case RunResult::Status::kFailed:
      return "failed";
    case RunResult::Status::kProtocolError:
      return "protocol_error";
    case RunResult::Status::kInternal:
      return "internal";
  }
  return "internal";
}

class ExecutionLog final {
 public:
  ExecutionLog(const std::string& name, const std::string& version, const std::string& command_id,
               const RunResult& result, const ExecutionState& state)
      : name_(name), version_(version), command_id_(command_id), result_(result), state_(state) {
    LogSkillEvent(spdlog::level::debug, "skill_execute_started",
                  {{"skill", name_}, {"version", version_}, {"command_id", command_id_}});
  }
  ~ExecutionLog() {
    LogSkillEvent(spdlog::level::debug, "skill_execute_completed",
                  {{"skill", name_},
                   {"version", version_},
                   {"runtime_id", state_.hooks.runtime_id},
                   {"execution_id", state_.hooks.execution_id},
                   {"command_id", command_id_},
                   {"status", RunStatusName(result_.status)},
                   {"accepted", result_.accepted},
                   {"reason", result_.reason_code},
                   {"message", result_.message},
                   {"primitive_calls", result_.primitive_acks.size()},
                   {"skill_logs", state_.logs_accepted},
                   {"skill_logs_dropped", state_.logs_dropped},
                   {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - started_)
                                       .count()}});
    if (state_.logs_dropped != 0) {
      LogSkillEvent(spdlog::level::warn, "skill_observability_suppressed",
                    {{"skill", name_},
                     {"version", version_},
                     {"command_id", command_id_},
                     {"kind", "diagnostic_log"},
                     {"dropped", state_.logs_dropped}});
    }
  }

 private:
  const std::string& name_;
  const std::string& version_;
  const std::string& command_id_;
  const RunResult& result_;
  const ExecutionState& state_;
  std::chrono::steady_clock::time_point started_{std::chrono::steady_clock::now()};
};

spdlog::level::level_enum LogLevel(eos_skill_log_level_v2 level) {
  switch (level) {
    case EOS_SKILL_LOG_TRACE:
      return spdlog::level::trace;
    case EOS_SKILL_LOG_DEBUG:
      return spdlog::level::debug;
    case EOS_SKILL_LOG_INFO:
      return spdlog::level::info;
    case EOS_SKILL_LOG_WARN:
      return spdlog::level::warn;
    case EOS_SKILL_LOG_ERROR:
      return spdlog::level::err;
    case EOS_SKILL_LOG_CRITICAL:
      return spdlog::level::critical;
    default:
      throw std::invalid_argument("invalid skill log level");
  }
}

bool AdmitSkillLog(ExecutionState& state) {
  constexpr double kRatePerSecond = 16.0;
  constexpr double kBurst = 64.0;
  constexpr std::size_t kMaximumAccepted = 1024;
  const auto now = std::chrono::steady_clock::now();
  state.log_tokens = std::min(
      kBurst, state.log_tokens +
                  std::chrono::duration<double>(now - state.log_refill).count() * kRatePerSecond);
  state.log_refill = now;
  if (state.logs_accepted >= kMaximumAccepted || state.log_tokens < 1.0) {
    ++state.logs_dropped;
    return false;
  }
  state.log_tokens -= 1.0;
  return true;
}

eos_skill_host_status_v2 WriteLog(void* opaque, const eos_skill_log_record_v2* record) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || record == nullptr || record->struct_size < sizeof(*record))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  if (!AdmitSkillLog(*state)) {
    if (state->hooks.diagnostic_log) state->hooks.diagnostic_log(false);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  std::string event;
  std::string message;
  nlohmann::json fields;
  try {
    event = CopyText(record->event_name, 128, "skill log event");
    message = CopyText(record->message, 1024, "skill log message", false);
    const std::string encoded = CopyText(record->fields_json, 16 * 1024, "skill log fields");
    if (!ValidName(event)) return EOS_SKILL_HOST_INVALID_ARGUMENT;
    fields = nlohmann::json::parse(encoded);
    if (!fields.is_object()) return EOS_SKILL_HOST_INVALID_ARGUMENT;
    (void)LogLevel(record->level);
  } catch (...) {
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  try {
    const Metadata& metadata = state->skill->metadata;
    LogSkillEvent(LogLevel(record->level), "skill_authored_log",
                  {{"skill", metadata.name},
                   {"version", metadata.version},
                   {"library", metadata.library},
                   {"library_version", metadata.library_version},
                   {"build_id", metadata.build_id},
                   {"runtime_id", state->hooks.runtime_id},
                   {"execution_id", state->hooks.execution_id},
                   {"command_id", state->command_id},
                   {"author_event", event},
                   {"message", message},
                   {"details", eos::lib::SanitizeLogFields(fields)}});
    ++state->logs_accepted;
    if (state->hooks.diagnostic_log) state->hooks.diagnostic_log(true);
    return EOS_SKILL_HOST_OK;
  } catch (...) {
    return EOS_SKILL_HOST_INTERNAL;
  }
}

eos_skill_host_status_v2 EmitCheckpoint(void* opaque, const eos_skill_checkpoint_v2* checkpoint) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || checkpoint == nullptr || checkpoint->struct_size < sizeof(*checkpoint))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  if (!state->hooks.emit_checkpoint) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    const std::string name = CopyText(checkpoint->name, 128, "checkpoint name");
    const std::string message = CopyText(checkpoint->message, 1024, "checkpoint message", false);
    const std::string encoded = CopyText(checkpoint->fields_json, 16 * 1024, "checkpoint fields");
    static const std::regex pattern("^[a-z][a-z0-9]*(?:\\.[a-z][a-z0-9_-]*)+$");
    if (!std::regex_match(name, pattern) || name.rfind("eos.", 0) == 0 ||
        name.rfind("execution.", 0) == 0 || checkpoint->has_progress > 1 ||
        (checkpoint->has_progress != 0 &&
         (!std::isfinite(checkpoint->progress) || checkpoint->progress < 0.0 ||
          checkpoint->progress > 1.0)))
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    nlohmann::json fields = nlohmann::json::parse(encoded);
    if (!fields.is_object()) return EOS_SKILL_HOST_INVALID_ARGUMENT;
    const Checkpoint value{
        name, message,
        checkpoint->has_progress ? std::optional<double>(checkpoint->progress) : std::nullopt,
        eos::lib::SanitizeLogFields(fields)};
    if (!state->hooks.emit_checkpoint(value)) return EOS_SKILL_HOST_UNAVAILABLE;
    const Metadata& metadata = state->skill->metadata;
    LogSkillEvent(
        spdlog::level::info, "skill_checkpoint",
        {{"skill", metadata.name},
         {"version", metadata.version},
         {"runtime_id", state->hooks.runtime_id},
         {"execution_id", state->hooks.execution_id},
         {"command_id", state->command_id},
         {"checkpoint", name},
         {"message", message},
         {"progress", value.progress ? nlohmann::json(*value.progress) : nlohmann::json(nullptr)},
         {"details", value.fields}});
    return EOS_SKILL_HOST_OK;
  } catch (...) {
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
}

eos_skill_host_status_v2 ReportFailure(void* opaque, const eos_skill_failure_v2* failure) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || failure == nullptr || failure->struct_size < sizeof(*failure))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  ++state->failure_reports;
  if (state->failure_reports != 1) {
    state->failure_report_error = "skill reported more than one terminal failure";
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  try {
    state->reported_failure_reason =
        CopyText(failure->reason_code, kMaxNameBytes, "failure reason_code");
    state->reported_failure_message = CopyText(failure->message, 1024, "failure message", false);
    if (!ValidName(state->reported_failure_reason))
      throw std::runtime_error("failure reason_code is invalid");
    return EOS_SKILL_HOST_OK;
  } catch (const std::exception& error) {
    state->reported_failure_reason.clear();
    state->reported_failure_message.clear();
    state->failure_report_error = error.what();
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
}

eos_skill_host_status_v2 HostStatus(dispatch::DispatchResult::Status status) {
  switch (status) {
    case dispatch::DispatchResult::Status::kCompleted:
      return EOS_SKILL_HOST_OK;
    case dispatch::DispatchResult::Status::kInvalidArgument:
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    case dispatch::DispatchResult::Status::kNotFound:
      return EOS_SKILL_HOST_NOT_FOUND;
    case dispatch::DispatchResult::Status::kAlreadyExists:
    case dispatch::DispatchResult::Status::kFailedPrecondition:
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    case dispatch::DispatchResult::Status::kUnavailable:
      return EOS_SKILL_HOST_UNAVAILABLE;
    case dispatch::DispatchResult::Status::kInternal:
      return EOS_SKILL_HOST_INTERNAL;
  }
  return EOS_SKILL_HOST_INTERNAL;
}

void EndAsyncActivity(ExecutionState& state, const AsyncPrimitive& operation,
                      const std::string& outcome) {
  if (!operation.activity_id.empty() && state.hooks.end_activity) {
    state.hooks.end_activity(operation.activity_id, outcome);
  }
}

eos_skill_host_status_v2 StartPrimitive(
    void* opaque, const eos_skill_primitive_request_v2* request,
    eos_skill_primitive_start_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || request == nullptr || response == nullptr ||
      request->struct_size < sizeof(eos_skill_primitive_request_v2) ||
      response->struct_size < sizeof(eos_skill_primitive_start_response_v2)) {
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  state->error.clear();
  response->operation_id = 0;
  response->error_message = {nullptr, 0};
  if (CheckCancelled(state)) {
    state->error = "skill execution is cancelled";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (state->release_latched ||
      state->async_primitives.size() >= kMaxAsyncPrimitivesPerExecution) {
    state->error = state->release_latched
                       ? "primitive release is latched after an earlier rejection"
                       : "too many asynchronous primitives are active";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  try {
    const std::string primitive = CopyText(request->primitive, kMaxNameBytes, "primitive");
    const std::string version = CopyText(request->version, kMaxVersionBytes, "primitive version");
    const std::string device_id = CopyText(request->device_id, kMaxNameBytes, "device_id", false);
    const std::string arguments_text =
        CopyText(request->arguments_json, kMaxJsonBytes, "arguments_json");
    const PrimitiveReference reference{primitive, version};
    if (std::find(state->skill->metadata.primitives.begin(),
                  state->skill->metadata.primitives.end(),
                  reference) == state->skill->metadata.primitives.end()) {
      state->error =
          "skill did not declare primitive '" + primitive + "' version '" + version + "'";
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    nlohmann::json arguments = nlohmann::json::parse(arguments_text);
    if (!arguments.is_object()) {
      state->error = "primitive arguments must be a JSON object";
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    const std::string child_command_id =
        state->command_id + "/" + std::to_string(state->call_index++);
    arguments["type"] = primitive;
    arguments["command_id"] = child_command_id;

    auto operation = std::make_unique<AsyncPrimitive>();
    operation->id = state->call_index;
    operation->command_id = child_command_id;
    operation->primitive = primitive;
    operation->version = version;
    operation->device_id = device_id;
    if (state->hooks.begin_activity) {
      operation->activity_id = state->hooks.begin_activity(
          {"primitive.call", {{"primitive", primitive}, {"version", version}}});
    }
    AsyncPrimitive* active = operation.get();
    try {
      const std::string correlation_id = eos::lib::CurrentCorrelationId();
      active->result = state->primitive_executor->Submit(
          [state, dispatcher = state->dispatcher, primitive, version, device_id,
           arguments = std::move(arguments), active, correlation_id]() mutable {
            std::optional<eos::lib::CorrelationScope> correlation;
            if (!correlation_id.empty()) correlation.emplace(correlation_id);
            if (state->hooks.acquire_device) {
              active->lease.release = state->hooks.acquire_device(device_id);
              if (!active->lease.release) {
                return dispatch::DispatchResult{dispatch::DispatchResult::Status::kUnavailable,
                                                {}, "skill execution was cancelled while waiting for device"};
              }
            }
             return dispatcher->ExecuteSkillStep(primitive, version, device_id, std::move(arguments),
                                                  [state, active] {
                                                    if (active->cancelled.load(
                                                            std::memory_order_relaxed))
                                                      return true;
                                                    try {
                                                      return state->cancelled && state->cancelled();
                                                    } catch (...) {
                                                      return true;
                                                    }
                                                  });
          });
    } catch (...) {
      EndAsyncActivity(*state, *operation, "failed");
      throw;
    }
    response->operation_id = operation->id;
    state->async_primitives.emplace(operation->id, std::move(operation));
    return EOS_SKILL_HOST_OK;
  } catch (const nlohmann::json::exception& error) {
    state->error = std::string("invalid primitive request JSON: ") + error.what();
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->error = error.what();
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) {
    state->error = "asynchronous primitive start failed";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_INTERNAL;
  }
}

eos_skill_host_status_v2 WaitPrimitive(void* opaque, std::uint64_t operation_id,
                                       std::uint32_t timeout_ms,
                                       eos_skill_primitive_wait_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || response == nullptr ||
      response->struct_size < sizeof(eos_skill_primitive_wait_response_v2)) {
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  state->acknowledgement.clear();
  state->error.clear();
  response->completed = 0;
  response->acknowledgement_json = {nullptr, 0};
  response->error_message = {nullptr, 0};
  const auto found = state->async_primitives.find(operation_id);
  if (found == state->async_primitives.end()) {
    state->error = "asynchronous primitive operation is not active";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_NOT_FOUND;
  }
  AsyncPrimitive& operation = *found->second;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (operation.result.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    if (CheckCancelled(state)) {
      operation.cancelled.store(true, std::memory_order_relaxed);
      try {
        (void)state->dispatcher->CancelCommand(operation.command_id);
      } catch (...) {
      }
      state->error = "skill execution is cancelled";
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return EOS_SKILL_HOST_OK;
    operation.result.wait_for(std::min(std::chrono::milliseconds(10),
                                      std::chrono::duration_cast<std::chrono::milliseconds>(
                                          deadline - now)));
  }

  dispatch::DispatchResult invoked;
  try {
    invoked = operation.result.get();
  } catch (const std::exception& error) {
    state->release_latched = true;
    state->error = std::string("primitive dispatcher failed: ") + error.what();
    RememberFailure(*state, "primitive.internal", "primitive dispatcher failed");
    response->error_message = Borrow(state->error);
    EndAsyncActivity(*state, operation, "failed");
    state->async_primitives.erase(found);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) {
    state->release_latched = true;
    state->error = "primitive dispatcher failed with an unknown exception";
    RememberFailure(*state, "primitive.internal", state->error);
    response->error_message = Borrow(state->error);
    EndAsyncActivity(*state, operation, "failed");
    state->async_primitives.erase(found);
    return EOS_SKILL_HOST_INTERNAL;
  }
  if (invoked.status != dispatch::DispatchResult::Status::kCompleted) {
    state->release_latched = true;
    state->error = invoked.message;
    RememberFailure(*state, "primitive.unavailable", state->error);
    response->error_message = Borrow(state->error);
    const auto status = HostStatus(invoked.status);
    EndAsyncActivity(*state, operation, "unavailable");
    state->async_primitives.erase(found);
    return status;
  }

  const PrimitiveAcknowledgement acknowledgement = ParsePrimitiveAcknowledgement(invoked.payload);
  if (acknowledgement.status == PrimitiveTerminalStatus::kInvalid) {
    state->release_latched = true;
    state->error = acknowledgement.message;
    RememberFailure(*state, "primitive.protocol_error", state->error);
    response->error_message = Borrow(state->error);
    EndAsyncActivity(*state, operation, "protocol_error");
    state->async_primitives.erase(found);
    return EOS_SKILL_HOST_INTERNAL;
  }
  state->acknowledgements.push_back(invoked.payload);
  state->acknowledgement = invoked.payload.dump();
  response->completed = 1;
  response->acknowledgement_json = Borrow(state->acknowledgement);
  const std::string terminal = invoked.payload.at("result").at("status").get<std::string>();
  if (acknowledgement.status != PrimitiveTerminalStatus::kSucceeded &&
      acknowledgement.status != PrimitiveTerminalStatus::kCancelled) {
    state->release_latched = true;
  }
  EndAsyncActivity(*state, operation, terminal);
  state->async_primitives.erase(found);
  return EOS_SKILL_HOST_OK;
}

eos_skill_host_status_v2 CancelPrimitive(void* opaque, std::uint64_t operation_id) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr) return EOS_SKILL_HOST_INVALID_ARGUMENT;
  const auto found = state->async_primitives.find(operation_id);
  if (found == state->async_primitives.end()) {
    return EOS_SKILL_HOST_NOT_FOUND;
  }
  AsyncPrimitive& operation = *found->second;
  operation.cancelled.store(true, std::memory_order_relaxed);
  const dispatch::DispatchResult cancelled = state->dispatcher->CancelCommand(operation.command_id);
  if (cancelled.status == dispatch::DispatchResult::Status::kNotFound) return EOS_SKILL_HOST_OK;
  return HostStatus(cancelled.status);
}

void DrainAsyncPrimitives(ExecutionState& state) {
  for (auto& [id, owned] : state.async_primitives) {
    (void)id;
    AsyncPrimitive& operation = *owned;
    operation.cancelled.store(true, std::memory_order_relaxed);
    try {
      (void)state.dispatcher->CancelCommand(operation.command_id);
    } catch (...) {
    }
  }
  for (auto& [id, owned] : state.async_primitives) {
    (void)id;
    AsyncPrimitive& operation = *owned;
    try {
      if (operation.result.valid()) (void)operation.result.get();
    } catch (...) {
    }
    EndAsyncActivity(state, operation, "cancelled");
  }
  state.async_primitives.clear();
}

eos_skill_host_status_v2 CallPrimitive(void* opaque, const eos_skill_primitive_request_v2* request,
                                       eos_skill_primitive_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || request == nullptr || response == nullptr ||
      request->struct_size < sizeof(eos_skill_primitive_request_v2) ||
      response->struct_size < sizeof(eos_skill_primitive_response_v2))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  state->acknowledgement.clear();
  state->error.clear();
  response->acknowledgement_json = {nullptr, 0};
  response->error_message = {nullptr, 0};
  if (CheckCancelled(state)) {
    state->error = "skill execution is cancelled";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (state->release_latched) {
    state->error = "primitive release is latched after an earlier rejection";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (!state->async_primitives.empty()) {
    state->error = "asynchronous primitives are active";
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  try {
    const std::string primitive = CopyText(request->primitive, kMaxNameBytes, "primitive");
    const std::string version = CopyText(request->version, kMaxVersionBytes, "primitive version");
    const std::string device_id = CopyText(request->device_id, kMaxNameBytes, "device_id", false);
    const std::string arguments_text =
        CopyText(request->arguments_json, kMaxJsonBytes, "arguments_json");
    const PrimitiveReference reference{primitive, version};
    if (std::find(state->skill->metadata.primitives.begin(),
                  state->skill->metadata.primitives.end(),
                  reference) == state->skill->metadata.primitives.end()) {
      state->error =
          "skill did not declare primitive '" + primitive + "' version '" + version + "'";
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    nlohmann::json arguments = nlohmann::json::parse(arguments_text);
    if (!arguments.is_object()) {
      state->error = "primitive arguments must be a JSON object";
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    arguments["type"] = primitive;
    const std::string child_command_id =
        state->command_id + "/" + std::to_string(state->call_index++);
    arguments["command_id"] = child_command_id;
    std::unique_ptr<ActivityScope> activity;
    dispatch::DispatchResult invoked;
    try {
      DeviceLease lease;
      if (state->hooks.acquire_device) {
        lease.release = state->hooks.acquire_device(device_id);
        if (!lease.release) {
          (void)CheckCancelled(state);
          state->error = "skill execution was cancelled while waiting for device";
          response->error_message = Borrow(state->error);
          return EOS_SKILL_HOST_UNAVAILABLE;
        }
      }
      activity = std::make_unique<ActivityScope>(
          *state, "primitive.call", nlohmann::json{{"primitive", primitive}, {"version", version}});
      invoked =
          state->dispatcher->ExecuteSkillStep(primitive, version, device_id, std::move(arguments),
                                              [state] { return CheckCancelled(state); });
    } catch (const std::exception& error) {
      state->release_latched = true;
      LogSkillEvent(spdlog::level::err, "skill_primitive_failed",
                    {{"primitive", primitive}, {"reason", "dispatcher_exception"}});
      state->error = std::string("primitive dispatcher failed: ") + error.what();
      RememberFailure(*state, "primitive.internal", "primitive dispatcher failed");
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INTERNAL;
    } catch (...) {
      state->release_latched = true;
      LogSkillEvent(spdlog::level::err, "skill_primitive_failed",
                    {{"primitive", primitive}, {"reason", "unknown_dispatcher_exception"}});
      state->error = "primitive dispatcher failed with an unknown exception";
      RememberFailure(*state, "primitive.internal", state->error);
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INTERNAL;
    }
    if (invoked.status != dispatch::DispatchResult::Status::kCompleted) {
      if (activity) activity->Outcome("unavailable");
      state->release_latched = true;
      state->error = invoked.message;
      RememberFailure(*state, "primitive.unavailable", state->error);
      response->error_message = Borrow(state->error);
      return HostStatus(invoked.status);
    }
    state->acknowledgements.push_back(invoked.payload);
    state->acknowledgement = invoked.payload.dump();
    response->acknowledgement_json = Borrow(state->acknowledgement);
    const PrimitiveAcknowledgement acknowledgement = ParsePrimitiveAcknowledgement(invoked.payload);
    if (acknowledgement.status == PrimitiveTerminalStatus::kInvalid) {
      if (activity) activity->Outcome("protocol_error");
      state->release_latched = true;
      state->error = acknowledgement.message;
      RememberFailure(*state, "primitive.protocol_error", state->error);
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_INTERNAL;
    }
    const bool was_accepted = invoked.payload.at("accepted").get<bool>();
    if (acknowledgement.status == PrimitiveTerminalStatus::kRejected) {
      state->release_latched = true;
      RememberFailure(*state, "primitive.rejected", "primitive request was rejected");
      if (activity) activity->Outcome("rejected");
    } else if (acknowledgement.status != PrimitiveTerminalStatus::kSucceeded) {
      state->release_latched = true;
      const std::string terminal = invoked.payload.at("result").at("status").get<std::string>();
      if (activity) activity->Outcome(terminal);
      state->error = acknowledgement.message.empty() ? "primitive execution " + terminal
                                                     : acknowledgement.message;
      RememberFailure(*state, "primitive." + terminal, state->error);
      response->error_message = Borrow(state->error);
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    LogSkillEvent(spdlog::level::trace, "skill_primitive_completed",
                  {{"skill", state->skill->metadata.name},
                   {"version", state->skill->metadata.version},
                   {"command_id", child_command_id},
                   {"primitive", primitive},
                   {"primitive_version", version},
                   {"accepted", was_accepted}});
    if (activity) activity->Outcome("succeeded");
    return EOS_SKILL_HOST_OK;
  } catch (const nlohmann::json::exception& error) {
    state->error = std::string("invalid primitive request JSON: ") + error.what();
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->error = error.what();
    response->error_message = Borrow(state->error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
}

eos_skill_host_status_v2 ReadResource(void* opaque, const eos_skill_resource_request_v2* request,
                                      eos_skill_resource_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || request == nullptr || response == nullptr ||
      request->struct_size < sizeof(eos_skill_resource_request_v2) ||
      response->struct_size < sizeof(eos_skill_resource_response_v2)) {
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  state->resource_value.clear();
  state->resource_error.clear();
  response->value_json = {nullptr, 0};
  response->error_message = {nullptr, 0};
  if (CheckCancelled(state)) {
    state->resource_error = "skill execution is cancelled";
    response->error_message = Borrow(state->resource_error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  try {
    const std::string uri = CopyText(request->uri, kMaxResourceTemplateBytes, "resource uri");
    const auto declared = std::find_if(
        state->skill->metadata.resources.begin(), state->skill->metadata.resources.end(),
        [&](const Resource& resource) {
          return ResourceTemplateMatches(resource.uri_template, uri);
        });
    if (declared == state->skill->metadata.resources.end()) {
      state->resource_error = "skill did not declare resource '" + uri + "'";
      RememberFailure(*state, "resource.not_declared", "skill requested an undeclared resource");
      response->error_message = Borrow(state->resource_error);
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    ActivityScope activity(*state, "resource.read", {{"template", declared->uri_template}});
    if (!state->resource_reader) {
      activity.Outcome("unavailable");
      state->resource_error = "resource provider is unavailable";
      RememberFailure(*state, "resource.unavailable", state->resource_error);
      response->error_message = Borrow(state->resource_error);
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    const nlohmann::json value = state->resource_reader(uri);
    if (!value.is_object()) {
      activity.Outcome("protocol_error");
      state->resource_error = "resource provider returned a non-object snapshot";
      RememberFailure(*state, "resource.protocol_error", state->resource_error);
      response->error_message = Borrow(state->resource_error);
      return EOS_SKILL_HOST_INTERNAL;
    }
    state->resource_value = value.dump();
    if (state->resource_value.size() > kMaxJsonBytes) {
      activity.Outcome("protocol_error");
      state->resource_value.clear();
      state->resource_error = "resource snapshot exceeds the size limit";
      RememberFailure(*state, "resource.protocol_error", state->resource_error);
      response->error_message = Borrow(state->resource_error);
      return EOS_SKILL_HOST_INTERNAL;
    }
    response->value_json = Borrow(state->resource_value);
    activity.Outcome("succeeded");
    return EOS_SKILL_HOST_OK;
  } catch (const std::exception& error) {
    state->resource_error = error.what();
    if (state->resource_error.size() > kMaxDescriptionBytes) {
      state->resource_error.resize(kMaxDescriptionBytes);
    }
    RememberFailure(*state, "resource.unavailable", state->resource_error);
    response->error_message = Borrow(state->resource_error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  } catch (...) {
    state->resource_error = "resource provider failed with an unknown exception";
    RememberFailure(*state, "resource.internal", state->resource_error);
    response->error_message = Borrow(state->resource_error);
    return EOS_SKILL_HOST_INTERNAL;
  }
}

std::uint32_t IsCancelled(void* opaque) {
  auto* state = static_cast<ExecutionState*>(opaque);
  return CheckCancelled(state) ? 1u : 0u;
}

eos_skill_signal_receive_status_v2 ReceiveSignal(void* opaque, std::uint32_t timeout_ms,
                                                 eos_skill_signal_v2* signal) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || signal == nullptr || signal->struct_size < sizeof(eos_skill_signal_v2))
    return EOS_SKILL_SIGNAL_RECEIVE_INVALID_ARGUMENT;
  signal->name = {nullptr, 0};
  signal->payload_json = {nullptr, 0};
  if (!state->hooks.receive_signal) {
    RememberFailure(*state, "signal.closed", "signal receiver is unavailable");
    return EOS_SKILL_SIGNAL_CLOSED;
  }
  try {
    std::optional<Signal> received =
        state->hooks.receive_signal(std::chrono::milliseconds(timeout_ms));
    if (!received)
      return CheckCancelled(state) ? EOS_SKILL_SIGNAL_CLOSED : EOS_SKILL_SIGNAL_TIMEOUT;
    const auto declared = std::find_if(
        state->skill->metadata.accepted_signals.begin(),
        state->skill->metadata.accepted_signals.end(),
        [&](const SignalDescriptor& descriptor) { return descriptor.name == received->name; });
    if ((declared == state->skill->metadata.accepted_signals.end() && received->name != "CANCEL") ||
        !received->payload.is_object()) {
      RememberFailure(*state, "signal.protocol_error", "received signal is invalid");
      return EOS_SKILL_SIGNAL_RECEIVE_INTERNAL;
    }
    if (received->name != "CANCEL") {
      state->skill->accepted_signal_validators.at(received->name)->validate(received->payload);
    }
    state->signal_name = received->name;
    state->signal_payload = received->payload.dump();
    if (state->signal_payload.size() > kMaxJsonBytes) {
      RememberFailure(*state, "signal.protocol_error", "received signal exceeds the size limit");
      return EOS_SKILL_SIGNAL_RECEIVE_INTERNAL;
    }
    signal->name = Borrow(state->signal_name);
    signal->payload_json = Borrow(state->signal_payload);
    return EOS_SKILL_SIGNAL_RECEIVED;
  } catch (...) {
    RememberFailure(*state, "signal.internal", "signal receive failed internally");
    return EOS_SKILL_SIGNAL_RECEIVE_INTERNAL;
  }
}

eos_skill_host_status_v2 EmitSignal(void* opaque, const eos_skill_signal_v2* signal) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || signal == nullptr || signal->struct_size < sizeof(eos_skill_signal_v2))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  if (CheckCancelled(state)) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    Signal emitted;
    emitted.name = CopyText(signal->name, kMaxNameBytes, "SIGNAL name");
    const auto declared = std::find_if(
        state->skill->metadata.emitted_signals.begin(),
        state->skill->metadata.emitted_signals.end(),
        [&](const SignalDescriptor& descriptor) { return descriptor.name == emitted.name; });
    if (declared == state->skill->metadata.emitted_signals.end()) {
      RememberFailure(*state, "signal.not_declared", "skill emitted an undeclared signal");
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    emitted.payload =
        nlohmann::json::parse(CopyText(signal->payload_json, kMaxJsonBytes, "SIGNAL payload_json"));
    if (!emitted.payload.is_object()) {
      RememberFailure(*state, "signal.invalid", "signal payload must be an object");
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    try {
      state->skill->emitted_signal_validators.at(emitted.name)->validate(emitted.payload);
    } catch (const std::exception&) {
      RememberFailure(*state, "signal.invalid", "signal payload violates its declared schema");
      return EOS_SKILL_HOST_INVALID_ARGUMENT;
    }
    if (!state->hooks.emit_signal) {
      RememberFailure(*state, "signal.unavailable", "signal delivery is unavailable");
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    if (!state->hooks.emit_signal(emitted)) {
      RememberFailure(*state, "signal.unavailable", "signal delivery was rejected");
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    return EOS_SKILL_HOST_OK;
  } catch (const nlohmann::json::exception&) {
    RememberFailure(*state, "signal.invalid", "signal payload is malformed");
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (...) {
    RememberFailure(*state, "signal.internal", "signal emit failed internally");
    return EOS_SKILL_HOST_INTERNAL;
  }
}

void InitializeSourceResponse(ExecutionState& state, eos_skill_source_response_v2* response) {
  state.source_topic.clear();
  state.source_value.clear();
  state.source_error.clear();
  response->has_sample = 0;
  response->sample = {sizeof(response->sample), {nullptr, 0}, 0, 0, {nullptr, 0}};
  response->error_message = {nullptr, 0};
}

const nlohmann::json_schema::json_validator& SourceValidator(const ExecutionState& state,
                                                              SourceAuthority authority,
                                                              const std::string& topic) {
  const auto found = state.skill->source_validators.find({authority, topic});
  if (found == state.skill->source_validators.end())
    throw std::invalid_argument("skill lacks source authority for topic '" + topic + "'");
  return *found->second;
}

void SetSourceResponse(ExecutionState& state, const SourceSample& sample,
                       const std::string& requested_topic,
                       const nlohmann::json_schema::json_validator& validator,
                       eos_skill_source_response_v2* response) {
  if (sample.topic != requested_topic) throw std::runtime_error("source provider returned wrong topic");
  if (!sample.value.is_object()) throw std::runtime_error("source provider returned non-object value");
  try {
    validator.validate(sample.value);
  } catch (const std::exception& error) {
    throw std::runtime_error(std::string("source provider returned value outside schema: ") +
                             error.what());
  }
  state.source_topic = sample.topic;
  state.source_value = sample.value.dump();
  if (state.source_topic.size() > kMaxNameBytes || !ValidName(state.source_topic) ||
      state.source_value.size() > kMaxJsonBytes)
    throw std::runtime_error("source provider returned an oversized or invalid sample");
  response->has_sample = 1;
  response->sample = {sizeof(response->sample), Borrow(state.source_topic),
                      sample.source_timestamp_ns, sample.sequence, Borrow(state.source_value)};
}

eos_skill_host_status_v2 PublishSource(void* opaque, const eos_skill_source_sample_v2* sample,
                                       eos_skill_source_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || response == nullptr || response->struct_size < sizeof(*response))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  InitializeSourceResponse(*state, response);
  if (CheckCancelled(state)) {
    state->source_error = "skill execution is cancelled";
    response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (sample == nullptr || sample->struct_size < sizeof(*sample))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  if (!state->hooks.publish_source) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    const std::string topic = CopyText(sample->topic, kMaxNameBytes, "source topic");
    nlohmann::json value = nlohmann::json::parse(CopyText(sample->value_json, kMaxJsonBytes,
                                                           "source value"));
    const auto& validator = SourceValidator(*state, SourceAuthority::kWrite, topic);
    if (!value.is_object()) throw std::invalid_argument("source value must be an object");
    validator.validate(value);
    SetSourceResponse(*state,
                      state->hooks.publish_source(topic, sample->source_timestamp_ns,
                                                  std::move(value)),
                      topic, validator, response);
    return EOS_SKILL_HOST_OK;
  } catch (const std::invalid_argument& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) { return EOS_SKILL_HOST_INTERNAL; }
}

eos_skill_host_status_v2 QuerySource(void* opaque, const eos_skill_source_query_v2* query,
                                     eos_skill_source_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || response == nullptr || response->struct_size < sizeof(*response))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  InitializeSourceResponse(*state, response);
  if (CheckCancelled(state)) {
    state->source_error = "skill execution is cancelled";
    response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (query == nullptr || query->struct_size < sizeof(*query) ||
      query->lookup > EOS_SKILL_SOURCE_NEAREST)
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  if (!state->hooks.query_source) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    const std::string topic = CopyText(query->topic, kMaxNameBytes, "source topic");
    const auto& validator = SourceValidator(*state, SourceAuthority::kRead, topic);
    auto sample = state->hooks.query_source(topic, static_cast<SourceLookup>(query->lookup),
                                             query->source_timestamp_ns);
    if (sample) SetSourceResponse(*state, *sample, topic, validator, response);
    return EOS_SKILL_HOST_OK;
  } catch (const std::invalid_argument& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) { return EOS_SKILL_HOST_INTERNAL; }
}

eos_skill_host_status_v2 WaitSource(void* opaque, eos_skill_text_v2 topic, std::uint64_t after,
                                    std::uint32_t timeout_ms,
                                    eos_skill_source_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || response == nullptr || response->struct_size < sizeof(*response))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  InitializeSourceResponse(*state, response);
  if (CheckCancelled(state)) {
    state->source_error = "skill execution is cancelled";
    response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_UNAVAILABLE;
  }
  if (!state->hooks.wait_source) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    const std::string copied_topic = CopyText(topic, kMaxNameBytes, "source topic");
    const auto& validator = SourceValidator(*state, SourceAuthority::kRead, copied_topic);
    auto sample = state->hooks.wait_source(copied_topic, after,
                                            std::chrono::milliseconds(timeout_ms));
    if (sample) SetSourceResponse(*state, *sample, copied_topic, validator, response);
    return CheckCancelled(state) ? EOS_SKILL_HOST_UNAVAILABLE : EOS_SKILL_HOST_OK;
  } catch (const std::invalid_argument& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->source_error = error.what(); response->error_message = Borrow(state->source_error);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) { return EOS_SKILL_HOST_INTERNAL; }
}

eos_skill_host_status_v2 InferOnce(void* opaque,
                                   const eos_skill_inference_request_v2* request,
                                   eos_skill_inference_response_v2* response) {
  auto* state = static_cast<ExecutionState*>(opaque);
  if (state == nullptr || request == nullptr || response == nullptr ||
      request->struct_size < sizeof(*request) || response->struct_size < sizeof(*response))
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  state->inference_result.clear();
  state->inference_error.clear();
  response->result_json = {nullptr, 0};
  response->error_message = {nullptr, 0};
  if (CheckCancelled(state)) return EOS_SKILL_HOST_UNAVAILABLE;
  try {
    if (request->authority < EOS_SKILL_INFERENCE_VAD ||
        request->authority > EOS_SKILL_INFERENCE_VLM)
      throw std::invalid_argument("invalid inference authority");
    InferenceRequest typed;
    typed.authority = static_cast<InferenceAuthority>(request->authority - 1);
    if (std::find(state->skill->metadata.inference_authorities.begin(),
                  state->skill->metadata.inference_authorities.end(), typed.authority) ==
        state->skill->metadata.inference_authorities.end())
      throw std::invalid_argument("skill did not declare inference authority");
    typed.source = CopyText(request->source, kMaxNameBytes, "inference source", false);
    typed.source_sequence = request->source_sequence;
    typed.source_generation = request->source_generation;
    typed.options = nlohmann::json::parse(
        CopyText(request->options_json, kMaxJsonBytes, "inference options"));
    if (!typed.options.is_object())
      throw std::invalid_argument("inference options must be an object");
    const bool has_any_provenance = !typed.source.empty() || typed.source_sequence != 0 ||
                                    typed.source_generation != 0;
    const bool has_exact_provenance = !typed.source.empty() && typed.source_sequence != 0 &&
                                      typed.source_generation != 0;
    const bool has_inline_media = typed.options.contains("input") ||
                                  typed.options.contains("depth_input");
    if (typed.authority == InferenceAuthority::kVad) {
      if (!typed.source.empty() || typed.source_sequence != 0 || typed.source_generation != 0 ||
          !typed.options.contains("input"))
        throw std::invalid_argument(
            "VAD inference requires caller-supplied input and empty source provenance");
    } else if (typed.authority == InferenceAuthority::kChatCompletion) {
      if (has_any_provenance || has_inline_media)
        throw std::invalid_argument(
            "chat completion requires text options and empty source provenance");
    } else if (typed.authority == InferenceAuthority::kVlm) {
      if (typed.options.contains("depth_input") ||
          (has_any_provenance && (!has_exact_provenance || has_inline_media)))
        throw std::invalid_argument(
            "VLM inference requires text with optional inline image or one exact retained frame");
    } else if (has_any_provenance == has_inline_media ||
               (has_any_provenance && !has_exact_provenance)) {
      throw std::invalid_argument(
          "vision inference requires either one exact retained frame or inline media");
    }
    if (!state->inference_runner) {
      state->inference_error = "inference capability is unavailable";
      response->error_message = Borrow(state->inference_error);
      return EOS_SKILL_HOST_UNAVAILABLE;
    }
    InferenceResult result =
        state->inference_runner(typed, [state] { return CheckCancelled(state); });
    if (result.authority != typed.authority || result.source != typed.source ||
        result.source_sequence != typed.source_sequence ||
        result.source_generation != typed.source_generation || !result.value.is_object())
      throw std::runtime_error("inference provider returned mismatched provenance");
    state->inference_result = result.value.dump();
    if (state->inference_result.size() > kMaxJsonBytes)
      throw std::runtime_error("inference result exceeds the size limit");
    response->result_json = Borrow(state->inference_result);
    return EOS_SKILL_HOST_OK;
  } catch (const std::invalid_argument& error) {
    state->inference_error = error.what();
    response->error_message = Borrow(state->inference_error);
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  } catch (const std::exception& error) {
    state->inference_error = error.what();
    response->error_message = Borrow(state->inference_error);
    return EOS_SKILL_HOST_INTERNAL;
  } catch (...) {
    return EOS_SKILL_HOST_INTERNAL;
  }
}

eos_skill_host_status_v2 Complete(void* opaque, const eos_skill_completion_v2* completion) {
  auto* state = static_cast<CompletionState*>(opaque);
  if (state == nullptr || completion == nullptr ||
      completion->struct_size < EOS_SKILL_COMPLETION_V2_BASE_SIZE)
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  ++state->calls;
  if (state->calls != 1) {
    state->valid = false;
    state->error = "skill completed more than once";
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
  try {
    if (completion->accepted > 1) throw std::runtime_error("accepted must be zero or one");
    state->accepted = completion->accepted == 1;
    state->reason_code = CopyText(completion->reason_code, kMaxNameBytes, "reason_code", false);
    if (EOS_SKILL_COMPLETION_V2_HAS(completion, message))
      state->message = CopyText(completion->message, 1024, "completion message", false);
    const std::string result_text =
        CopyText(completion->result_json, kMaxJsonBytes, "result_json", false);
    state->result =
        result_text.empty() ? nlohmann::json::object() : nlohmann::json::parse(result_text);
    if (!state->result.is_object()) throw std::runtime_error("result_json must be an object");
    state->valid = true;
    return EOS_SKILL_HOST_OK;
  } catch (const std::exception& error) {
    state->error = error.what();
    return EOS_SKILL_HOST_INVALID_ARGUMENT;
  }
}
}  // namespace

void Runtime::ValidateArguments(const std::string& name, const std::string& version,
                                const nlohmann::json& arguments) const {
  if (!arguments.is_object()) throw std::invalid_argument("skill arguments must be an object");
  const auto found = impl_->skills_by_identity.find({name, version});
  if (found == impl_->skills_by_identity.end()) {
    throw std::invalid_argument("skill is not admitted: " + name + "@" + version);
  }
  const detail::LoadedSkill* selected = found->second;
  if (arguments.dump().size() > detail::kMaxJsonBytes) {
    throw std::invalid_argument("skill arguments exceed the size limit");
  }
  std::lock_guard<std::mutex> lock(*selected->execution_mutex);
  try {
    selected->parameters_validator->validate(arguments);
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string("skill arguments violate parameters schema: ") +
                                error.what());
  }
}

RunResult Runtime::Execute(const std::string& name, const std::string& version,
                           const nlohmann::json& arguments, const std::string& command_id,
                           Cancelled cancelled) const {
  ExecutionHooks hooks;
  hooks.cancelled = std::move(cancelled);
  return ExecuteWithHooks(name, version, arguments, command_id, std::move(hooks));
}

RunResult Runtime::ExecuteWithHooks(const std::string& name, const std::string& version,
                                    const nlohmann::json& arguments, const std::string& command_id,
                                    ExecutionHooks hooks) const {
  auto& logger = SkillLogger();
  RunResult result;
  if (command_id.empty()) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skill_execute_rejected",
                       {{"skill", name}, {"version", version}, {"reason", "missing_command_id"}});
    result.status = RunResult::Status::kInvalidArgument;
    result.message = "skill command_id is required";
    return result;
  }
  if (!arguments.is_object()) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skill_execute_rejected",
                       {{"skill", name}, {"version", version}, {"reason", "arguments_not_object"}});
    result.status = RunResult::Status::kInvalidArgument;
    result.message = "skill arguments must be an object";
    return result;
  }
  const auto found = impl_->skills_by_identity.find({name, version});
  if (found == impl_->skills_by_identity.end()) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skill_execute_rejected",
                       {{"skill", name}, {"version", version}, {"reason", "not_loaded"}});
    result.status = RunResult::Status::kFailed;
    result.reason_code = "not_found";
    result.message = "skill is not loaded";
    return result;
  }
  const detail::LoadedSkill* selected = found->second;
  std::unique_lock<std::mutex> lock(*selected->execution_mutex, std::defer_lock);
  if (!selected->metadata.reentrant) lock.lock();
  try {
    selected->parameters_validator->validate(arguments);
  } catch (const std::exception& error) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skill_execute_rejected",
                       {{"skill", name}, {"version", version}, {"reason", "schema_validation"}});
    result.status = RunResult::Status::kInvalidArgument;
    result.reason_code = "arguments.invalid";
    result.message = std::string("skill arguments violate parameters schema: ") + error.what();
    return result;
  }
  ExecutionState execution_state;
  execution_state.skill = selected;
  execution_state.dispatcher = impl_->dispatcher;
  execution_state.primitive_executor = &impl_->primitive_executor;
  execution_state.cancelled = hooks.cancelled;
  execution_state.hooks = std::move(hooks);
  execution_state.command_id = command_id;
  execution_state.resource_reader = impl_->resource_reader;
  execution_state.inference_runner = impl_->inference_runner;
  CompletionState completion_state;
  const std::string arguments_text = arguments.dump();
  if (arguments_text.size() > detail::kMaxJsonBytes) {
    eos::lib::LogEvent(logger, spdlog::level::warn, "skill_execute_rejected",
                       {{"skill", name},
                        {"version", version},
                        {"reason", "arguments_too_large"},
                        {"argument_bytes", arguments_text.size()}});
    result.status = RunResult::Status::kInvalidArgument;
    result.message = "skill arguments exceed the size limit";
    return result;
  }
  ExecutionLog execution_log(name, version, command_id, result, execution_state);
  eos_skill_execute_status_v2 status = EOS_SKILL_EXECUTE_FAILED;
  try {
    eos_skill_host_v2 host{EOS_SKILL_ABI_VERSION_V2,
                           sizeof(eos_skill_host_v2),
                           &execution_state,
                           CallPrimitive,
                           IsCancelled,
                           ReadResource,
                           ReceiveSignal,
                            EmitSignal,
                            WriteLog,
                            EmitCheckpoint,
                            ReportFailure,
                            StartPrimitive,
                            WaitPrimitive,
                             CancelPrimitive,
                             PublishSource,
                             QuerySource,
                             WaitSource,
                             InferOnce};
    eos_skill_execution_v2 execution{sizeof(eos_skill_execution_v2), detail::Borrow(command_id),
                                     detail::Borrow(arguments_text), &host};
    status = selected->descriptor_v2->execute(selected->descriptor_v2->context, &execution,
                                               Complete, &completion_state);
  } catch (...) {
    DrainAsyncPrimitives(execution_state);
    result.status = RunResult::Status::kInternal;
    result.reason_code = "skill.exception";
    result.message = "skill allowed an exception to cross the native ABI";
    result.primitive_acks = std::move(execution_state.acknowledgements);
    return result;
  }
  const bool abandoned_primitive = !execution_state.async_primitives.empty();
  DrainAsyncPrimitives(execution_state);
  if (abandoned_primitive && status != EOS_SKILL_EXECUTE_CANCELLED &&
      !execution_state.cancellation_latched) {
    execution_state.failure_report_error =
        "skill returned with an outstanding asynchronous primitive";
  }
  result.primitive_acks = std::move(execution_state.acknowledgements);
  if (!execution_state.failure_report_error.empty()) {
    result.status = RunResult::Status::kProtocolError;
    result.reason_code = "skill.protocol_error";
    result.message = execution_state.failure_report_error;
    return result;
  }
  if (execution_state.cancellation_latched) {
    result.status = RunResult::Status::kCancelled;
    result.reason_code = "cancelled";
    return result;
  }
  if (status == EOS_SKILL_EXECUTE_INVALID_ARGUMENT) {
    if (completion_state.calls != 0) {
      result.status = RunResult::Status::kProtocolError;
      result.reason_code = "skill.protocol_error";
      result.message = "skill completed but returned a non-success execution status";
      return result;
    }
    result.status = RunResult::Status::kInvalidArgument;
    result.reason_code = execution_state.reported_failure_reason.empty()
                             ? "arguments.invalid"
                             : execution_state.reported_failure_reason;
    result.message = execution_state.reported_failure_message.empty()
                         ? "skill rejected its invocation arguments"
                         : execution_state.reported_failure_message;
    return result;
  }
  if (status == EOS_SKILL_EXECUTE_CANCELLED) {
    if (completion_state.calls != 0) {
      result.status = RunResult::Status::kProtocolError;
      result.reason_code = "skill.protocol_error";
      result.message = "skill completed but returned a non-success execution status";
      return result;
    }
    result.status = RunResult::Status::kCancelled;
    result.reason_code = "cancelled";
    return result;
  }
  if (status != EOS_SKILL_EXECUTE_OK) {
    if (completion_state.calls != 0) {
      result.status = RunResult::Status::kProtocolError;
      result.reason_code = "skill.protocol_error";
      result.message = "skill completed but returned a non-success execution status";
      return result;
    }
    result.status = RunResult::Status::kFailed;
    result.reason_code =
        !execution_state.reported_failure_reason.empty() ? execution_state.reported_failure_reason
        : !execution_state.last_failure_reason.empty()   ? execution_state.last_failure_reason
                                                         : "skill.failed";
    result.message = !execution_state.reported_failure_message.empty()
                         ? execution_state.reported_failure_message
                         : execution_state.last_failure_message;
    return result;
  }
  if (completion_state.calls != 1 || !completion_state.valid) {
    result.status = RunResult::Status::kProtocolError;
    result.reason_code = "skill.protocol_error";
    result.message = completion_state.error.empty() ? "skill did not complete exactly once"
                                                    : completion_state.error;
    return result;
  }
  try {
    selected->returns_validator->validate(completion_state.result);
  } catch (const std::exception& error) {
    result.status = RunResult::Status::kProtocolError;
    result.reason_code = "skill.protocol_error";
    result.message = std::string("skill result violates returns schema: ") + error.what();
    return result;
  }
  result.status = RunResult::Status::kCompleted;
  result.accepted = completion_state.accepted && !execution_state.release_latched;
  result.reason_code = execution_state.release_latched ? "primitive.rejected"
                       : completion_state.reason_code.empty()
                           ? (result.accepted ? "completed" : "rejected")
                           : completion_state.reason_code;
  result.result = std::move(completion_state.result);
  result.message = std::move(completion_state.message);
  if (execution_state.release_latched && !execution_state.last_failure_reason.empty()) {
    result.reason_code = execution_state.last_failure_reason;
    if (result.message.empty()) result.message = execution_state.last_failure_message;
  }
  return result;
}
}  // namespace eos::runtime::skills::native

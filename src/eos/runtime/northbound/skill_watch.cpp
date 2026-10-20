#include <chrono>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/northbound/log_status.h"
#include "src/eos/runtime/northbound/request_context.h"
#include "src/eos/runtime/northbound/skill_codec.h"
#include "src/eos/runtime/northbound/skill_service.h"
#include "src/eos/runtime/skills/manager/manager.h"

namespace eos::runtime::northbound {
namespace {

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("northbound");
  return logger;
}

}  // namespace

grpc::Status SkillGrpcService::FollowExecutionJournal(
    grpc::ServerContext* context, const island::eos::v1::FollowExecutionJournalRequest* request,
    grpc::ServerWriter<island::eos::v1::SkillJournalEntry>* writer) {
  const auto started = std::chrono::steady_clock::now();
  std::string request_id;
  const grpc::Status request_status = ResolveRequestId(context, &request_id);
  eos::lib::CorrelationScope scope(request_id);
  std::size_t sent = 0;
  std::uint64_t after = request->after_journal_sequence();
  const auto finish = [&](grpc::Status status, const char* stage, bool terminal,
                          bool disconnected = false) {
    const bool cancelled = disconnected || context->IsCancelled();
    eos::lib::LogEvent(Logger(), cancelled || status.ok() ? spdlog::level::debug
                                                         : spdlog::level::warn,
                       "skill_event_watch_closed",
                       {{"service", "SkillService"},
                        {"method", "FollowExecutionJournal"},
                        {"execution_id", request->execution_id()},
                        {"status", static_cast<int>(status.error_code())},
                        {"status_name", GrpcStatusName(status.error_code())},
                        {"reason_category", GrpcReasonCategory(status.error_code())},
                        {"stage", stage},
                        {"terminal", terminal},
                        {"cancelled", cancelled},
                        {"events", sent},
                        {"last_journal_sequence", after},
                        {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count()}},
                       status.ok() ? "" : std::string("FollowExecutionJournal failed: ") +
                                              GrpcStatusName(status.error_code()));
    return status;
  };

  if (!request_status.ok()) return finish(request_status, "metadata", false);
  if (request->execution_id().empty()) {
    return finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required"),
                  "validation", false);
  }
  if (!service_.Get(request->execution_id())) {
    return finish(grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found"), "lookup",
                  false);
  }

  const auto terminal_at_cursor = [&] {
    const auto execution = service_.Get(request->execution_id());
    return execution && skills::IsTerminal(execution->status) &&
           execution->latest_journal_sequence <= after;
  };
  if (terminal_at_cursor()) return finish(grpc::Status::OK, "terminal", true);

  eos::lib::LogEvent(Logger(), spdlog::level::debug, "skill_event_watch_opened",
                     {{"service", "SkillService"},
                      {"method", "FollowExecutionJournal"},
                      {"execution_id", request->execution_id()},
                      {"after_journal_sequence", after}});
  while (!context->IsCancelled()) {
    const auto events = service_.WaitForJournal(request->execution_id(), after,
                                                std::chrono::milliseconds(250));
    for (const auto& event : events) {
      island::eos::v1::SkillJournalEntry encoded;
      grpc::Status status = FillJournalEntry(event, &encoded);
      if (!status.ok()) return finish(status, "encode", false);
      if (!writer->Write(encoded)) return finish(grpc::Status::OK, "write", false, true);
      after = event.sequence;
      ++sent;
      if (event.kind == skills::JournalEntry::Kind::kLifecycle &&
          skills::IsTerminal(event.execution.status)) {
        return finish(grpc::Status::OK, "terminal", true);
      }
    }
    if (terminal_at_cursor()) return finish(grpc::Status::OK, "terminal", true);
  }
  return finish(grpc::Status::OK, "cancelled", false);
}

grpc::Status SkillGrpcService::WatchExecutionState(
    grpc::ServerContext* context, const island::eos::v1::WatchExecutionStateRequest* request,
    grpc::ServerWriter<island::eos::v1::SkillExecutionState>* writer) {
  std::string request_id;
  const grpc::Status request_status = ResolveRequestId(context, &request_id);
  eos::lib::CorrelationScope scope(request_id);
  if (!request_status.ok()) return request_status;
  if (request->execution_id().empty())
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required");
  if (!service_.Get(request->execution_id()))
    return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found");

  std::uint64_t after = request->after_revision();
  while (!context->IsCancelled()) {
    const auto state = service_.WaitForExecutionState(request->execution_id(), after,
                                                       std::chrono::milliseconds(250));
    if (state) {
      island::eos::v1::SkillExecutionState encoded;
      const grpc::Status status = FillExecutionState(request->execution_id(), *state, &encoded);
      if (!status.ok()) return status;
      if (!writer->Write(encoded)) return grpc::Status::OK;
      after = state->revision;
    }
    const auto execution = service_.Get(request->execution_id());
    if (!execution || (skills::IsTerminal(execution->status) && !state)) return grpc::Status::OK;
  }
  return grpc::Status::OK;
}

grpc::Status SkillGrpcService::MonitorExecutions(
    grpc::ServerContext* context, const island::eos::v1::MonitorExecutionsRequest* request,
    grpc::ServerWriter<island::eos::v1::SkillMonitorUpdate>* writer) {
  skills::ExecutionSelector selector;
  if (request->has_selector() && request->selector().has_reference()) {
    selector.skill_name = request->selector().reference().name();
    selector.skill_version = request->selector().reference().version();
  }
  if (request->has_selector()) {
    for (const auto status : request->selector().statuses()) {
      switch (status) {
        case island::eos::v1::EXECUTION_STATUS_QUEUED:
          selector.statuses.push_back(skills::ExecutionStatus::kQueued);
          break;
        case island::eos::v1::EXECUTION_STATUS_RUNNING:
          selector.statuses.push_back(skills::ExecutionStatus::kRunning);
          break;
        case island::eos::v1::EXECUTION_STATUS_SUCCEEDED:
          selector.statuses.push_back(skills::ExecutionStatus::kSucceeded);
          break;
        case island::eos::v1::EXECUTION_STATUS_REJECTED:
          selector.statuses.push_back(skills::ExecutionStatus::kRejected);
          break;
        case island::eos::v1::EXECUTION_STATUS_CANCELLED:
          selector.statuses.push_back(skills::ExecutionStatus::kCancelled);
          break;
        case island::eos::v1::EXECUTION_STATUS_TIMED_OUT:
          selector.statuses.push_back(skills::ExecutionStatus::kTimedOut);
          break;
        case island::eos::v1::EXECUTION_STATUS_FAILED:
          selector.statuses.push_back(skills::ExecutionStatus::kFailed);
          break;
        default:
          return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                              "selector statuses must be specified");
      }
    }
  }
  if (!selector.skill_version.empty() && selector.skill_name.empty())
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "skill version requires skill name");
  std::uint64_t after = request->after_monitor_sequence();
  try {
    while (!context->IsCancelled()) {
      const auto updates = service_.WaitForMonitor(selector, after, std::chrono::milliseconds(250));
      for (const auto& update : updates) {
        island::eos::v1::SkillMonitorUpdate encoded;
        const auto status = FillMonitorUpdate(update, &encoded);
        if (!status.ok()) return status;
        if (!writer->Write(encoded)) return grpc::Status::OK;
        after = update.sequence;
      }
    }
  } catch (const std::invalid_argument& error) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, error.what());
  }
  return grpc::Status::OK;
}

}  // namespace eos::runtime::northbound

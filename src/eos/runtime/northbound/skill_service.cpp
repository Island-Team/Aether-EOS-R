#include "src/eos/runtime/northbound/skill_service.h"

#include <google/protobuf/util/time_util.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "src/eos/lib/log.h"
#include "src/eos/runtime/northbound/duration.h"
#include "src/eos/runtime/northbound/log_status.h"
#include "src/eos/runtime/northbound/request_context.h"
#include "src/eos/runtime/northbound/skill_codec.h"
#include "src/eos/runtime/skills/manager/manager.h"

namespace eos::runtime::northbound {
namespace {

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("northbound");
  return logger;
}

template <typename Handler>
grpc::Status Handle(grpc::ServerContext* context, const char* method, Handler&& handler) {
  std::string request_id;
  grpc::Status status = ResolveRequestId(context, &request_id);
  eos::lib::CorrelationScope scope(request_id);
  const auto started = std::chrono::steady_clock::now();
  try {
    if (status.ok()) status = handler();
  } catch (const std::out_of_range& error) {
    status = grpc::Status(grpc::StatusCode::NOT_FOUND, error.what());
  } catch (const std::length_error& error) {
    status = grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, error.what());
  } catch (const std::invalid_argument& error) {
    const std::string message = error.what();
    status = grpc::Status(message.find("not admitted") == std::string::npos
                              ? grpc::StatusCode::INVALID_ARGUMENT
                              : grpc::StatusCode::NOT_FOUND,
                          message);
  } catch (const std::logic_error& error) {
    status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, error.what());
  } catch (const std::runtime_error& error) {
    const std::string message = error.what();
    status = grpc::Status(message.find("does not support SIGNAL") == std::string::npos
                              ? grpc::StatusCode::RESOURCE_EXHAUSTED
                              : grpc::StatusCode::UNAVAILABLE,
                          message);
  } catch (const std::exception& error) {
    status = grpc::Status(grpc::StatusCode::INTERNAL, error.what());
  }
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
  const nlohmann::json fields{{"service", "SkillService"},
                              {"method", method},
                              {"status", static_cast<int>(status.error_code())},
                              {"status_name", GrpcStatusName(status.error_code())},
                              {"reason_category", GrpcReasonCategory(status.error_code())},
                              {"duration_ms", duration}};
  eos::lib::LogEvent(Logger(), status.ok() ? spdlog::level::debug : spdlog::level::warn,
                     status.ok() ? "grpc_request_completed" : "grpc_request_rejected", fields,
                     status.ok() ? "" : std::string(method) + " failed: " +
                                            GrpcStatusName(status.error_code()));
  return status;
}

}  // namespace

grpc::Status SkillGrpcService::ListSkills(grpc::ServerContext* context,
                                          const island::eos::v1::ListSkillsRequest*,
                                          island::eos::v1::ListSkillsResponse* response) {
  return Handle(context, "ListSkills", [&] {
    for (const auto& metadata : service_.ListSkills()) {
      const grpc::Status status = FillSkillDescriptor(metadata, response->add_skills());
      if (!status.ok()) return status;
    }
    return grpc::Status::OK;
  });
}

grpc::Status SkillGrpcService::GetSkill(grpc::ServerContext* context,
                                        const island::eos::v1::GetSkillRequest* request,
                                        island::eos::v1::SkillDescriptor* response) {
  return Handle(context, "GetSkill", [&] {
    if (!request->has_reference() || request->reference().name().empty() ||
        request->reference().version().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "exact skill reference is required");
    const auto* metadata =
        service_.GetSkill(request->reference().name(), request->reference().version());
    if (metadata == nullptr)
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "skill is not admitted");
    return FillSkillDescriptor(*metadata, response);
  });
}

grpc::Status SkillGrpcService::StartExecution(grpc::ServerContext* context,
                                              const island::eos::v1::StartExecutionRequest* request,
                                              island::eos::v1::SkillExecution* response) {
  return Handle(context, "StartExecution", [&] {
    if (!request->has_reference() || !request->has_arguments())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "reference and arguments are required");
    if (request->has_timeout() && !IsValidDuration(request->timeout()))
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "timeout is invalid");
    skills::StartOptions options;
    if (request->has_timeout())
      options.timeout = std::chrono::milliseconds(
          google::protobuf::util::TimeUtil::DurationToMilliseconds(request->timeout()));
    // Registration waits for three people to speak and has no ASR deadline. A short
    // execution deadline can leave faces registered before cancelling the skill.
    if (request->reference().name() == "register_host" && options.timeout &&
        *options.timeout < std::chrono::minutes(5))
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "register_host timeout must be at least 5 minutes; omit timeout "
                          "to wait without an execution deadline");
    const skills::StartResult result =
        service_.Start(request->reference().name(), request->reference().version(),
                       SkillStructJson(request->arguments()), request->request_id(),
                       options);
    if (result.status == skills::StartResult::Status::kConflict)
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                          "request_id is already bound to a different execution request");
    return FillSkillExecution(result.execution, response);
  });
}

grpc::Status SkillGrpcService::GetExecution(grpc::ServerContext* context,
                                            const island::eos::v1::GetExecutionRequest* request,
                                            island::eos::v1::SkillExecution* response) {
  return Handle(context, "GetExecution", [&] {
    if (request->execution_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required");
    const auto execution = service_.Get(request->execution_id());
    if (!execution) return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found");
    return FillSkillExecution(*execution, response);
  });
}

grpc::Status SkillGrpcService::GetExecutionState(
    grpc::ServerContext* context, const island::eos::v1::GetExecutionStateRequest* request,
    island::eos::v1::SkillExecutionState* response) {
  return Handle(context, "GetExecutionState", [&] {
    if (request->execution_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required");
    if (!service_.Get(request->execution_id()))
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found");
    const auto state = service_.GetExecutionState(request->execution_id());
    if (!state)
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution state is not available");
    return FillExecutionState(request->execution_id(), *state, response);
  });
}

grpc::Status SkillGrpcService::TerminateExecution(
    grpc::ServerContext* context, const island::eos::v1::TerminateExecutionRequest* request,
    island::eos::v1::SkillExecution* response) {
  return Handle(context, "TerminateExecution", [&] {
    if (request->execution_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required");
    if (!service_.Terminate(request->execution_id()))
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found");
    return FillSkillExecution(*service_.Get(request->execution_id()), response);
  });
}

grpc::Status SkillGrpcService::TerminateExecutions(
    grpc::ServerContext* context, const island::eos::v1::TerminateExecutionsRequest* request,
    island::eos::v1::TerminateExecutionsResponse* response) {
  return Handle(context, "TerminateExecutions", [&] {
    if (!request->has_reference() || request->reference().name().empty() ||
        request->request_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "skill name and request_id are required");
    const auto result = service_.TerminateMany(request->reference().name(),
                                               request->reference().version(),
                                               request->request_id());
    if (result.status == skills::TerminateManyResult::Status::kConflict)
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                          "request_id is already bound to another termination selector");
    response->set_request_id(result.request_id);
    for (const auto& id : result.execution_ids) response->add_execution_ids(id);
    return grpc::Status::OK;
  });
}

grpc::Status SkillGrpcService::WatchExecution(
    grpc::ServerContext* context, const island::eos::v1::WatchExecutionRequest* request,
    grpc::ServerWriter<island::eos::v1::SkillExecution>* writer) {
  return Handle(context, "WatchExecution", [&] {
    if (request->execution_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "execution_id is required");
    const auto current = service_.Get(request->execution_id());
    if (!current)
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "execution is not found");
    if (skills::IsTerminal(current->status) && current->sequence <= request->after_sequence())
      return grpc::Status::OK;
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t after = request->after_sequence();
    std::size_t sent = 0;
    eos::lib::LogEvent(Logger(), spdlog::level::debug, "skill_watch_opened",
                       {{"execution_id", request->execution_id()}, {"after_sequence", after}});
    while (!context->IsCancelled()) {
      const auto snapshots =
          service_.WaitForSnapshots(request->execution_id(), after, std::chrono::milliseconds(250));
      for (const auto& snapshot : snapshots) {
        island::eos::v1::SkillExecution encoded;
        grpc::Status status = FillSkillExecution(snapshot, &encoded);
        if (!status.ok()) return status;
        if (!writer->Write(encoded)) {
          const bool cancelled = context->IsCancelled();
          eos::lib::LogEvent(
              Logger(), cancelled ? spdlog::level::debug : spdlog::level::warn,
              cancelled ? "skill_watch_closed" : "skill_watch_failed",
              {{"execution_id", request->execution_id()},
               {"stage", "write"},
               {"terminal", false},
               {"cancelled", cancelled},
               {"snapshots", sent},
               {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count()}});
          return grpc::Status::OK;
        }
        after = snapshot.sequence;
        ++sent;
        eos::lib::LogEvent(Logger(), spdlog::level::trace, "skill_watch_snapshot_sent",
                           {{"execution_id", snapshot.execution_id},
                            {"request_id", snapshot.request_id},
                            {"status", skills::ExecutionStatusName(snapshot.status)},
                            {"sequence", snapshot.sequence}});
        if (skills::IsTerminal(snapshot.status)) {
          eos::lib::LogEvent(Logger(), spdlog::level::debug, "skill_watch_closed",
                             {{"execution_id", snapshot.execution_id},
                              {"terminal", true},
                              {"cancelled", false},
                              {"snapshots", sent},
                              {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now() - started)
                                                  .count()}});
          return grpc::Status::OK;
        }
      }
    }
    eos::lib::LogEvent(Logger(), spdlog::level::debug, "skill_watch_closed",
                       {{"execution_id", request->execution_id()},
                        {"terminal", false},
                        {"cancelled", true},
                        {"snapshots", sent},
                        {"duration_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::steady_clock::now() - started)
                                            .count()}});
    return grpc::Status::OK;
  });
}

grpc::Status SkillGrpcService::ListExecutions(grpc::ServerContext* context,
                                              const island::eos::v1::ListExecutionsRequest*,
                                              island::eos::v1::ListExecutionsResponse* response) {
  return Handle(context, "ListExecutions", [&] {
    for (const auto& execution : service_.ListExecutions()) {
      const grpc::Status status = FillSkillExecution(execution, response->add_executions());
      if (!status.ok()) return status;
    }
    return grpc::Status::OK;
  });
}

grpc::Status SkillGrpcService::SignalExecution(
    grpc::ServerContext* context, const island::eos::v1::SignalExecutionRequest* request,
    island::eos::v1::SkillSignalReceipt* response) {
  return Handle(context, "SignalExecution", [&] {
    if (request->execution_id().empty() || request->request_id().empty() ||
        request->name().empty() || !request->has_payload()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "execution_id, request_id, name, and payload are required");
    }
    const skills::SignalReceipt receipt =
        service_.SignalExecution(request->execution_id(), request->name(),
                                 SkillStructJson(request->payload()), request->request_id());
    if (receipt.status == skills::SignalReceipt::Status::kConflict) {
      return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                          "request_id is already bound to a different SIGNAL");
    }
    response->set_signal_id(receipt.signal_id);
    response->set_execution_id(receipt.execution_id);
    response->set_request_id(receipt.request_id);
    response->set_accepted(true);
    return grpc::Status::OK;
  });
}

}  // namespace eos::runtime::northbound

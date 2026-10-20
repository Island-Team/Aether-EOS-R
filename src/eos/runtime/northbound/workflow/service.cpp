#include "src/eos/runtime/northbound/workflow/service.h"

#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/time_util.h>

#include <chrono>
#include <memory>
#include <stdexcept>

#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/northbound/duration.h"
#include "src/eos/runtime/northbound/log_status.h"
#include "src/eos/runtime/northbound/request_context.h"
#include "src/eos/runtime/northbound/skill_codec.h"
#include "src/eos/runtime/skills/manager/manager.h"

namespace eos::runtime::northbound {
namespace {
namespace Proto = island::eos::v1;

std::shared_ptr<spdlog::logger>& Logger() {
  static auto logger = eos::lib::GetLogger("northbound");
  return logger;
}

nlohmann::json DefinitionJson(const Proto::WorkflowDefinition& definition) {
  nlohmann::json output{{"workflow_id", definition.workflow_id()},
                        {"actions", nlohmann::json::array()},
                        {"supersedes_workflow_ids", nlohmann::json::array()}};
  for (const auto& workflow_id : definition.supersedes_workflow_ids())
    output["supersedes_workflow_ids"].push_back(workflow_id);
  for (const auto& action : definition.actions()) {
    if (!action.has_parameters() || action.action_id().empty() ||
        action.target_case() == Proto::WorkflowAction::TARGET_NOT_SET)
      throw std::invalid_argument("each action requires an ID, target, and parameters");
    nlohmann::json encoded{{"action_id", action.action_id()},
                           {"parameters", SkillStructJson(action.parameters())},
                           {"depends_on", nlohmann::json::array()},
                           {"output_bindings", nlohmann::json::array()}};
    if (action.has_skill())
      encoded["skill"] = {{"name", action.skill().name()}, {"version", action.skill().version()}};
    else if (action.has_primitive())
      encoded["primitive"] = {{"name", action.primitive().name()},
                              {"version", action.primitive().version()},
                              {"device_id", action.primitive().device_id()}};
    else if (action.has_control())
      encoded["control"] = {{"target_action_id", action.control().target_action_id()},
                            {"operation", "terminate"}};
    else
      encoded["noop"] = nlohmann::json::object();
    if (!action.choice_group().empty()) encoded["choice_group"] = action.choice_group();
    if (action.execution_mode() == Proto::ACTION_EXECUTION_MODE_TASK)
      encoded["execution_mode"] = "task";
    else if (action.execution_mode() == Proto::ACTION_EXECUTION_MODE_MONITOR)
      encoded["execution_mode"] = "monitor";
    if (action.monitor_lifetime() == Proto::MONITOR_LIFETIME_EXPLICIT)
      encoded["monitor_lifetime"] = "explicit";
    else if (action.monitor_lifetime() == Proto::MONITOR_LIFETIME_WORKFLOW)
      encoded["monitor_lifetime"] = "workflow";
    if (action.finalizer()) encoded["finalizer"] = true;
    if (action.continue_on_failure()) encoded["continue_on_failure"] = true;
    for (const auto& dependency : action.depends_on()) encoded["depends_on"].push_back(dependency);
    for (const auto& binding : action.output_bindings())
      encoded["output_bindings"].push_back(
          {{"source_action_id", binding.source_action_id()},
           {"source_output_pointer", binding.source_output_pointer()},
           {"destination_parameter_pointer", binding.destination_parameter_pointer()}});
    if (action.has_when()) {
      if (action.when().resource_uri().empty()) {
        if (action.when().fact_key().empty() || !action.when().has_equals())
          throw std::invalid_argument("state condition requires fact_key and equals");
        std::string json;
        const auto status =
            google::protobuf::util::MessageToJsonString(action.when().equals(), &json);
        if (!status.ok()) throw std::invalid_argument("state condition value is invalid");
        encoded["when"] = {{"fact", action.when().fact_key()},
                           {"equals", nlohmann::json::parse(json)}};
        if (!action.when().source_action_id().empty())
          encoded["when"]["source_action_id"] = action.when().source_action_id();
      } else {
        if (!action.when().fact_key().empty() || !action.when().source_action_id().empty() ||
            action.when().detection_kind().empty() ||
            action.when().detection_value_pointer().empty() || !action.when().has_equals())
          throw std::invalid_argument(
              "resource state condition requires resource_uri, detection_kind, "
              "detection_value_pointer, and equals");
        encoded["when"] = {{"resource", action.when().resource_uri()},
                           {"detection_kind", action.when().detection_kind()},
                           {"detection_value_pointer",
                            action.when().detection_value_pointer()}};
        std::string json;
        const auto status =
            google::protobuf::util::MessageToJsonString(action.when().equals(), &json);
        if (!status.ok()) throw std::invalid_argument("state condition value is invalid");
        encoded["when"]["equals"] = nlohmann::json::parse(json);
      }
      if (action.when().has_max_age()) {
        if (!IsValidDuration(action.when().max_age()))
          throw std::invalid_argument("state condition max_age is invalid");
        const auto max_age =
            google::protobuf::util::TimeUtil::DurationToMilliseconds(action.when().max_age());
        if (max_age < 0)
          throw std::invalid_argument("state condition max_age must not be negative");
        encoded["when"]["max_age_ms"] = max_age;
      }
    }
    output["actions"].push_back(std::move(encoded));
  }
  return output;
}

Proto::ActionStatus ActionStatus(const std::string& status) {
  if (status == "blocked") return Proto::ACTION_STATUS_BLOCKED;
  if (status == "ready") return Proto::ACTION_STATUS_READY;
  if (status == "queued") return Proto::ACTION_STATUS_QUEUED;
  if (status == "running") return Proto::ACTION_STATUS_RUNNING;
  if (status == "succeeded") return Proto::ACTION_STATUS_SUCCEEDED;
  if (status == "rejected") return Proto::ACTION_STATUS_REJECTED;
  if (status == "failed") return Proto::ACTION_STATUS_FAILED;
  if (status == "cancelled") return Proto::ACTION_STATUS_CANCELLED;
  if (status == "timed_out") return Proto::ACTION_STATUS_TIMED_OUT;
  if (status == "outcome_unknown") return Proto::ACTION_STATUS_OUTCOME_UNKNOWN;
  if (status == "skipped") return Proto::ACTION_STATUS_SKIPPED;
  if (status == "stopped") return Proto::ACTION_STATUS_STOPPED;
  return Proto::ACTION_STATUS_UNSPECIFIED;
}

Proto::ActionListStatus ActionListStatus(const std::string& status) {
  if (status == "pending") return Proto::ACTION_LIST_STATUS_PENDING;
  if (status == "running") return Proto::ACTION_LIST_STATUS_RUNNING;
  if (status == "succeeded") return Proto::ACTION_LIST_STATUS_SUCCEEDED;
  if (status == "failed") return Proto::ACTION_LIST_STATUS_FAILED;
  if (status == "superseding") return Proto::ACTION_LIST_STATUS_SUPERSEDING;
  if (status == "superseded") return Proto::ACTION_LIST_STATUS_SUPERSEDED;
  return Proto::ACTION_LIST_STATUS_UNSPECIFIED;
}

grpc::Status FillActionList(const nlohmann::json& source, Proto::ActionList* output) {
  output->set_workflow_id(source.at("workflow_id").get<std::string>());
  output->set_revision(source.at("revision").get<std::uint64_t>());
  output->set_status(ActionListStatus(source.at("status").get<std::string>()));
  if (source.contains("supersedes_workflow_ids"))
    for (const auto& workflow_id : source.at("supersedes_workflow_ids"))
      output->add_supersedes_workflow_ids(workflow_id.get<std::string>());
  if (source.contains("superseded_by_workflow_id"))
    output->set_superseded_by_workflow_id(
        source.at("superseded_by_workflow_id").get<std::string>());
  output->mutable_created_at()->CopyFrom(google::protobuf::util::TimeUtil::MillisecondsToTimestamp(
      source.at("created_at_unix_ms").get<std::int64_t>()));
  output->mutable_updated_at()->CopyFrom(google::protobuf::util::TimeUtil::MillisecondsToTimestamp(
      source.at("updated_at_unix_ms").get<std::int64_t>()));
  for (const auto& action : source.at("actions")) {
    auto* item = output->add_actions();
    auto* encoded = item->mutable_action();
    encoded->set_action_id(action.at("action_id").get<std::string>());
    if (action.contains("skill")) {
      encoded->mutable_skill()->set_name(action.at("skill").at("name").get<std::string>());
      encoded->mutable_skill()->set_version(action.at("skill").at("version").get<std::string>());
    } else if (action.contains("primitive")) {
      encoded->mutable_primitive()->set_name(action.at("primitive").at("name").get<std::string>());
      encoded->mutable_primitive()->set_version(
          action.at("primitive").at("version").get<std::string>());
      encoded->mutable_primitive()->set_device_id(
          action.at("primitive").at("device_id").get<std::string>());
    } else if (action.contains("control")) {
      encoded->mutable_control()->set_target_action_id(
          action.at("control").at("target_action_id").get<std::string>());
      if (action.at("control").value("operation", "") == "terminate")
        encoded->mutable_control()->set_operation(Proto::ACTION_CONTROL_OPERATION_TERMINATE);
    } else if (action.contains("noop")) {
      encoded->mutable_noop();
    }
    if (action.contains("choice_group"))
      encoded->set_choice_group(action.at("choice_group").get<std::string>());
    if (action.value("execution_mode", "") == "task")
      encoded->set_execution_mode(Proto::ACTION_EXECUTION_MODE_TASK);
    else if (action.value("execution_mode", "") == "monitor")
      encoded->set_execution_mode(Proto::ACTION_EXECUTION_MODE_MONITOR);
    if (action.value("monitor_lifetime", "") == "explicit")
      encoded->set_monitor_lifetime(Proto::MONITOR_LIFETIME_EXPLICIT);
    else if (action.value("monitor_lifetime", "") == "workflow")
      encoded->set_monitor_lifetime(Proto::MONITOR_LIFETIME_WORKFLOW);
    encoded->set_finalizer(action.value("finalizer", false));
    encoded->set_continue_on_failure(action.value("continue_on_failure", false));
    grpc::Status status = FillSkillStruct(action.at("parameters"), encoded->mutable_parameters());
    if (!status.ok()) return status;
    for (const auto& dependency : action.at("depends_on"))
      encoded->add_depends_on(dependency.get<std::string>());
    for (const auto& binding : action.value("output_bindings", nlohmann::json::array())) {
      auto* output_binding = encoded->add_output_bindings();
      output_binding->set_source_action_id(binding.at("source_action_id").get<std::string>());
      output_binding->set_source_output_pointer(
          binding.at("source_output_pointer").get<std::string>());
      output_binding->set_destination_parameter_pointer(
          binding.at("destination_parameter_pointer").get<std::string>());
    }
    if (action.contains("when")) {
      auto* condition = encoded->mutable_when();
      if (action.at("when").contains("resource")) {
        condition->set_resource_uri(action.at("when").at("resource").get<std::string>());
        condition->set_detection_kind(
            action.at("when").at("detection_kind").get<std::string>());
        condition->set_detection_value_pointer(
            action.at("when").at("detection_value_pointer").get<std::string>());
        const auto value_status = google::protobuf::util::JsonStringToMessage(
            action.at("when").at("equals").dump(), condition->mutable_equals());
        if (!value_status.ok())
          return grpc::Status(grpc::StatusCode::INTERNAL,
                              "cannot encode resource condition value");
      } else {
        condition->set_fact_key(action.at("when").at("fact").get<std::string>());
        if (action.at("when").contains("source_action_id"))
          condition->set_source_action_id(
              action.at("when").at("source_action_id").get<std::string>());
        const auto value_status = google::protobuf::util::JsonStringToMessage(
            action.at("when").at("equals").dump(), condition->mutable_equals());
        if (!value_status.ok())
          return grpc::Status(grpc::StatusCode::INTERNAL,
                              "cannot encode state condition value");
      }
      if (action.at("when").contains("max_age_ms")) {
        condition->mutable_max_age()->CopyFrom(
            google::protobuf::util::TimeUtil::MillisecondsToDuration(
                action.at("when").at("max_age_ms").get<std::int64_t>()));
      }
    }
    item->set_status(ActionStatus(action.at("status").get<std::string>()));
    if (action.contains("execution_id")) item->set_execution_id(action.at("execution_id"));
    if (action.contains("command_id")) item->set_command_id(action.at("command_id"));
    if (action.contains("result")) {
      status = FillSkillStruct(action.at("result"), item->mutable_result());
      if (!status.ok()) return status;
    }
    if (action.contains("resolved_parameters")) {
      status =
          FillSkillStruct(action.at("resolved_parameters"), item->mutable_resolved_parameters());
      if (!status.ok()) return status;
    }
    if (action.contains("output")) {
      const auto output_status = google::protobuf::util::JsonStringToMessage(
          action.at("output").dump(), item->mutable_output());
      if (!output_status.ok())
        return grpc::Status(grpc::StatusCode::INTERNAL, "cannot encode action output");
    }
    if (action.contains("reason")) item->set_reason(action.at("reason").get<std::string>());
    if (action.contains("message")) item->set_message(action.at("message").get<std::string>());
  }
  return grpc::Status::OK;
}

google::protobuf::Timestamp TimestampFromMillis(std::int64_t millis) {
  return google::protobuf::util::TimeUtil::MillisecondsToTimestamp(millis);
}

grpc::Status FillRuntimeSnapshot(const actionlist::RuntimeSnapshot& source,
                                 Proto::ManagerRuntimeSnapshot* output) {
  const auto captured_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(source.captured_at.time_since_epoch())
          .count();
  output->mutable_captured_at()->CopyFrom(TimestampFromMillis(captured_ms));
  output->mutable_action_list_cursor()->set_epoch(source.cursor.epoch);
  output->mutable_action_list_cursor()->set_sequence(source.cursor.sequence);
  for (const auto& execution : source.executions) {
    grpc::Status status = FillSkillExecution(execution, output->add_executions());
    if (!status.ok()) return status;
  }
  for (const auto& [key, fact] : source.facts) {
    auto* encoded = output->add_facts();
    encoded->set_key(key);
    const auto value_status =
        google::protobuf::util::JsonStringToMessage(fact.value.dump(), encoded->mutable_value());
    if (!value_status.ok()) return {grpc::StatusCode::INTERNAL, "cannot encode state fact value"};
    encoded->set_source(fact.source);
    encoded->set_source_execution_id(fact.source);
    encoded->set_source_sequence(fact.sequence);
    const auto event_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(fact.event_time.time_since_epoch())
            .count();
    const auto received_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(fact.received_time.time_since_epoch())
            .count();
    encoded->mutable_event_time()->CopyFrom(TimestampFromMillis(event_ms));
    encoded->mutable_received_time()->CopyFrom(TimestampFromMillis(received_ms));
  }
  for (const auto& action_list : source.action_lists) {
    grpc::Status status = FillActionList(action_list, output->add_action_lists());
    if (!status.ok()) return status;
  }
  return grpc::Status::OK;
}

grpc::Status FillEvent(const actionlist::Event& source, Proto::ActionListEvent* output) {
  output->mutable_cursor()->set_epoch(source.cursor.epoch);
  output->mutable_cursor()->set_sequence(source.cursor.sequence);
  const auto observed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(source.observed_at.time_since_epoch())
          .count();
  output->mutable_observed_at()->CopyFrom(TimestampFromMillis(observed_ms));
  if (source.kind == actionlist::Event::Kind::kActionList)
    return FillActionList(source.action_list, output->mutable_action_list());
  auto* fact = output->mutable_fact();
  fact->set_key(source.fact_key);
  const auto value_status =
      google::protobuf::util::JsonStringToMessage(source.fact.value.dump(), fact->mutable_value());
  if (!value_status.ok()) return {grpc::StatusCode::INTERNAL, "cannot encode state fact value"};
  fact->set_source(source.fact.source);
  fact->set_source_execution_id(source.fact.source);
  fact->set_source_sequence(source.fact.sequence);
  const auto event_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            source.fact.event_time.time_since_epoch())
                            .count();
  const auto received_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               source.fact.received_time.time_since_epoch())
                               .count();
  fact->mutable_event_time()->CopyFrom(TimestampFromMillis(event_ms));
  fact->mutable_received_time()->CopyFrom(TimestampFromMillis(received_ms));
  return grpc::Status::OK;
}

template <typename Handler>
grpc::Status HandleWorkflow(grpc::ServerContext* context, const char* method, Handler&& handler) {
  std::string request_id;
  grpc::Status status = ResolveRequestId(context, &request_id);
  eos::lib::CorrelationScope scope(request_id);
  const auto started = std::chrono::steady_clock::now();
  try {
    if (status.ok()) status = handler();
  } catch (const std::invalid_argument& error) {
    status = {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  } catch (const std::logic_error& error) {
    status = {grpc::StatusCode::FAILED_PRECONDITION, error.what()};
  } catch (const std::exception& error) {
    status = {grpc::StatusCode::INTERNAL, error.what()};
  }
  const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - started)
                            .count();
  const nlohmann::json fields{{"service", "WorkflowService"},
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

WorkflowGrpcService::WorkflowGrpcService(actionlist::Manager& manager,
                                         const std::filesystem::path& template_directory)
    : manager_(manager), registry_(template_directory) {}

grpc::Status WorkflowGrpcService::ListWorkflowTemplates(
    grpc::ServerContext* context, const Proto::ListWorkflowTemplatesRequest*,
    Proto::ListWorkflowTemplatesResponse* response) {
  return HandleWorkflow(context, "ListWorkflowTemplates", [&] {
    for (const auto& descriptor : registry_.List()) *response->add_templates() = descriptor;
    return grpc::Status::OK;
  });
}

grpc::Status WorkflowGrpcService::GetWorkflowTemplate(
    grpc::ServerContext* context, const Proto::GetWorkflowTemplateRequest* request,
    Proto::WorkflowTemplateDescriptor* response) {
  return HandleWorkflow(context, "GetWorkflowTemplate", [&] {
    if (request->name().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "template name is required");
    const auto descriptor = registry_.Get(request->name());
    if (!descriptor)
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "workflow template not found");
    *response = *descriptor;
    return grpc::Status::OK;
  });
}

grpc::Status WorkflowGrpcService::RunWorkflowTemplate(
    grpc::ServerContext* context, const Proto::RunWorkflowTemplateRequest* request,
    Proto::ActionList* response) {
  return HandleWorkflow(context, "RunWorkflowTemplate", [&] {
    if (request->name().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "template name is required");
    const auto descriptor = registry_.Get(request->name());
    if (!descriptor)
      return grpc::Status(grpc::StatusCode::NOT_FOUND, "workflow template not found");
    Proto::WorkflowDefinition definition = descriptor->definition();
    definition.set_workflow_id(
        request->workflow_id().empty()
            ? definition.workflow_id() + "-run-" + eos::lib::MintUuidV7()
            : request->workflow_id());
    const auto applied = manager_.Submit(DefinitionJson(definition));
    return FillActionList(applied, response);
  });
}

grpc::Status WorkflowGrpcService::ApplyActionList(grpc::ServerContext* context,
                                                  const Proto::ApplyActionListRequest* request,
                                                  Proto::ActionList* response) {
  return HandleWorkflow(context, "ApplyActionList", [&] {
    if (!request->has_definition() || request->definition().workflow_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "workflow definition with workflow_id is required");
    const auto applied = manager_.Submit(DefinitionJson(request->definition()));
    return FillActionList(applied, response);
  });
}

grpc::Status WorkflowGrpcService::GetActionList(grpc::ServerContext* context,
                                                const Proto::GetActionListRequest* request,
                                                Proto::ActionList* response) {
  return HandleWorkflow(context, "GetActionList", [&] {
    if (request->workflow_id().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "workflow_id is required");
    const auto action_list = manager_.Get(request->workflow_id());
    if (!action_list) return grpc::Status(grpc::StatusCode::NOT_FOUND, "ActionList not found");
    return FillActionList(*action_list, response);
  });
}

grpc::Status WorkflowGrpcService::ListActionLists(grpc::ServerContext* context,
                                                  const Proto::ListActionListsRequest*,
                                                  Proto::ListActionListsResponse* response) {
  return HandleWorkflow(context, "ListActionLists", [&] {
    for (const auto& action_list : manager_.List()) {
      grpc::Status status = FillActionList(action_list, response->add_action_lists());
      if (!status.ok()) return status;
    }
    return grpc::Status::OK;
  });
}

grpc::Status WorkflowGrpcService::GetRuntimeSnapshot(grpc::ServerContext* context,
                                                     const Proto::GetRuntimeSnapshotRequest*,
                                                     Proto::ManagerRuntimeSnapshot* response) {
  return HandleWorkflow(context, "GetRuntimeSnapshot",
                        [&] { return FillRuntimeSnapshot(manager_.Snapshot(), response); });
}

grpc::Status WorkflowGrpcService::WatchActionListEvents(
    grpc::ServerContext* context, const Proto::WatchActionListEventsRequest* request,
    grpc::ServerWriter<Proto::ActionListEvent>* writer) {
  return HandleWorkflow(context, "WatchActionListEvents", [&] {
    if (!request->has_after() || request->after().epoch().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "after cursor is required");
    actionlist::EventCursor cursor{request->after().epoch(), request->after().sequence()};
    while (!context->IsCancelled()) {
      auto batch = manager_.WaitForEvents(cursor, std::chrono::milliseconds(250));
      if (batch.status == actionlist::ReplayStatus::kEpochMismatch)
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "ActionList cursor epoch does not match this EOS boot");
      if (batch.status == actionlist::ReplayStatus::kExpired)
        return grpc::Status(grpc::StatusCode::OUT_OF_RANGE,
                            "ActionList cursor is older than retained events");
      if (batch.status == actionlist::ReplayStatus::kFuture)
        return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                            "ActionList cursor is ahead of this EOS runtime");
      for (const auto& event : batch.events) {
        Proto::ActionListEvent encoded;
        grpc::Status status = FillEvent(event, &encoded);
        if (!status.ok()) return status;
        if (!writer->Write(encoded)) return grpc::Status::OK;
        cursor = event.cursor;
      }
    }
    return grpc::Status::OK;
  });
}

}  // namespace eos::runtime::northbound

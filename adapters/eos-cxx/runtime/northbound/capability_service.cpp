#include "adapters/eos-cxx/runtime/northbound/capability_service.h"

#include <google/protobuf/util/json_util.h>

#include <exception>
#include <string>
#include <utility>

#include "adapters/eos-cxx/lib/log.h"
#include "adapters/eos-cxx/runtime/dispatch/dispatcher.h"
#include "adapters/eos-cxx/runtime/kernel/broker.h"
#include "adapters/eos-cxx/runtime/northbound/request_context.h"

namespace eos::runtime::northbound {
namespace {

using Result = dispatch::DispatchResult;
using Status = Result::Status;
using Operation = island::eos::v2::Operation;
using OperationStatus = island::eos::v2::OperationStatus;

grpc::Status AsStatus(const Result& result) {
  switch (result.status) {
    case Status::kPending:
    case Status::kCompleted:
      return grpc::Status::OK;
    case Status::kInvalidArgument:
      return {grpc::StatusCode::INVALID_ARGUMENT, result.message};
    case Status::kNotFound:
      return {grpc::StatusCode::NOT_FOUND, result.message};
    case Status::kAlreadyExists:
      return {grpc::StatusCode::ALREADY_EXISTS, result.message};
    case Status::kFailedPrecondition:
      return {grpc::StatusCode::FAILED_PRECONDITION, result.message};
    case Status::kUnavailable:
      return {grpc::StatusCode::UNAVAILABLE, result.message};
    case Status::kInternal:
      return {grpc::StatusCode::INTERNAL, result.message};
  }
  return {grpc::StatusCode::INTERNAL, "unknown dispatch status"};
}

OperationStatus TerminalStatus(const std::string& status) {
  if (status == "succeeded") return island::eos::v2::OPERATION_STATUS_SUCCEEDED;
  if (status == "rejected") return island::eos::v2::OPERATION_STATUS_REJECTED;
  if (status == "failed") return island::eos::v2::OPERATION_STATUS_FAILED;
  if (status == "cancelled") return island::eos::v2::OPERATION_STATUS_CANCELLED;
  if (status == "timed_out") return island::eos::v2::OPERATION_STATUS_TIMED_OUT;
  if (status == "outcome_unknown") return island::eos::v2::OPERATION_STATUS_OUTCOME_UNKNOWN;
  return island::eos::v2::OPERATION_STATUS_UNSPECIFIED;
}

grpc::Status FillOperation(const Result& result, const std::string& operation_id,
                           Operation* response) {
  const grpc::Status code = AsStatus(result);
  if (!code.ok()) return code;
  response->set_operation_id(operation_id);
  if (result.status == Status::kPending) {
    response->set_status(island::eos::v2::OPERATION_STATUS_PENDING);
    return grpc::Status::OK;
  }
  if (!result.payload.is_object() || !result.payload.contains("result") ||
      !result.payload.at("result").is_object())
    return {grpc::StatusCode::INTERNAL, "terminal operation has no acknowledgement"};
  const auto status = TerminalStatus(result.payload.at("result").value("status", ""));
  if (status == island::eos::v2::OPERATION_STATUS_UNSPECIFIED)
    return {grpc::StatusCode::INTERNAL, "terminal operation has an unknown status"};
  response->set_status(status);
  response->set_accepted(result.payload.value("accepted", false));
  const auto reason = result.payload.find("reason_code");
  if (reason != result.payload.end() && reason->is_string())
    response->set_reason_code(reason->get<std::string>());
  const auto parsed = google::protobuf::util::JsonStringToMessage(
      result.payload.dump(), response->mutable_acknowledgement());
  if (!parsed.ok()) return {grpc::StatusCode::INTERNAL, "cannot encode operation acknowledgement"};
  return grpc::Status::OK;
}

grpc::Status ValidateId(const std::string& id) {
  if (id.empty() || id.size() > 128)
    return {grpc::StatusCode::INVALID_ARGUMENT, "operation_id must contain 1..128 bytes"};
  return grpc::Status::OK;
}

template <typename Handler>
grpc::Status Handle(grpc::ServerContext* context, Handler&& handler) {
  std::string request_id;
  const grpc::Status status = ResolveRequestId(context, &request_id);
  if (!status.ok()) return status;
  lib::CorrelationScope scope(request_id);
  try {
    return handler();
  } catch (const std::exception&) {
    return {grpc::StatusCode::INTERNAL, "capability operation failed"};
  }
}

}  // namespace

grpc::Status CapabilityGrpcService::ListCapabilities(
    grpc::ServerContext* context, const island::eos::v2::ListCapabilitiesRequest*,
    island::eos::v2::ListCapabilitiesResponse* response) {
  return Handle(context, [&] {
    for (const auto& capability : broker_.catalog().List()) {
      auto* descriptor = response->add_capabilities();
      descriptor->set_name(capability.name);
      descriptor->set_available(broker_.catalog().Available(capability.name));
      descriptor->set_primitive(capability.primitive);
      descriptor->set_version(capability.version);
      descriptor->set_device_id(capability.device_id);
    }
    return grpc::Status::OK;
  });
}

grpc::Status CapabilityGrpcService::SubmitOperation(
    grpc::ServerContext* context, const island::eos::v2::SubmitOperationRequest* request,
    Operation* response) {
  return Handle(context, [&] {
    const auto id_status = ValidateId(request->operation_id());
    if (!id_status.ok()) return id_status;
    if (request->capability_name().empty())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "capability_name is required");
    if (!request->has_arguments())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "arguments are required");
    std::string text;
    const auto serialized =
        google::protobuf::util::MessageToJsonString(request->arguments(), &text);
    if (!serialized.ok())
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "arguments cannot be decoded");
    return FillOperation(
        broker_.StartTrustedCapability(request->capability_name(), nlohmann::json::parse(text),
                                       request->operation_id()),
        request->operation_id(), response);
  });
}

grpc::Status CapabilityGrpcService::GetOperation(grpc::ServerContext* context,
                                                 const island::eos::v2::OperationRequest* request,
                                                 Operation* response) {
  return Handle(context, [&] {
    const auto id_status = ValidateId(request->operation_id());
    if (!id_status.ok()) return id_status;
    return FillOperation(dispatcher_.GetCommand(request->operation_id()), request->operation_id(),
                         response);
  });
}

grpc::Status CapabilityGrpcService::WatchOperation(grpc::ServerContext* context,
                                                   const island::eos::v2::OperationRequest* request,
                                                   grpc::ServerWriter<Operation>* writer) {
  return Handle(context, [&] {
    const auto id_status = ValidateId(request->operation_id());
    if (!id_status.ok()) return id_status;
    Operation current;
    const auto current_status = FillOperation(dispatcher_.GetCommand(request->operation_id()),
                                              request->operation_id(), &current);
    if (!current_status.ok()) return current_status;
    if (!writer->Write(current) || current.status() != island::eos::v2::OPERATION_STATUS_PENDING)
      return grpc::Status::OK;
    Operation terminal;
    const auto terminal_status =
        FillOperation(dispatcher_.WaitCommand(request->operation_id(),
                                              [context] { return context->IsCancelled(); }),
                      request->operation_id(), &terminal);
    if (!terminal_status.ok()) return terminal_status;
    if (!context->IsCancelled() && terminal.status() != island::eos::v2::OPERATION_STATUS_PENDING)
      writer->Write(terminal);
    return grpc::Status::OK;
  });
}

grpc::Status CapabilityGrpcService::CancelOperation(
    grpc::ServerContext* context, const island::eos::v2::OperationRequest* request,
    Operation* response) {
  return Handle(context, [&] {
    const auto id_status = ValidateId(request->operation_id());
    if (!id_status.ok()) return id_status;
    return FillOperation(dispatcher_.CancelCommand(request->operation_id()),
                         request->operation_id(), response);
  });
}

}  // namespace eos::runtime::northbound

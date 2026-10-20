#include "src/eos/runtime/models/grpc_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <grpcpp/grpcpp.h>
#include <thread>

#include "model_provider.grpc.pb.h"

namespace eos::runtime::models {
namespace {
namespace protocol = island::eos::external::models::v1;

CallResult Failure(const ModelConfig& model, const grpc::Status& status) {
  CallResult::Status code = CallResult::Status::kRemoteError;
  switch (status.error_code()) {
    case grpc::StatusCode::CANCELLED:
    case grpc::StatusCode::UNAVAILABLE:
      code = CallResult::Status::kUnavailable;
      break;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
      code = CallResult::Status::kDeadlineExceeded;
      break;
    case grpc::StatusCode::INVALID_ARGUMENT:
      code = CallResult::Status::kInvalidArgument;
      break;
    default:
      break;
  }
  return {code, 0, {}, "model '" + model.id + "' gRPC failed: " + status.error_message()};
}

template <typename Invoke>
CallResult Request(const ModelConfig& model, std::chrono::milliseconds timeout,
                   const CancelRequested& cancel_requested, Invoke invoke) {
  if (cancel_requested && cancel_requested()) {
    return {CallResult::Status::kUnavailable, 0, {}, "model '" + model.id + "' request was cancelled"};
  }
  grpc::ChannelArguments channel_options;
  channel_options.SetMaxReceiveMessageSize(
      static_cast<int>(std::min<std::size_t>(model.max_response_bytes, INT_MAX - 1024) + 1024));
  channel_options.SetMaxSendMessageSize(
      static_cast<int>(std::min<std::size_t>(model.max_request_bytes, INT_MAX - 1024) + 1024));
  auto channel = grpc::CreateCustomChannel(model.endpoint, grpc::InsecureChannelCredentials(),
                                           channel_options);
  auto client = protocol::ModelProviderService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  std::atomic<bool> done{false};
  std::thread cancellation;
  if (cancel_requested) {
    cancellation = std::thread([&] {
      while (!done.load()) {
        if (cancel_requested()) {
          context.TryCancel();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }
  CallResult result = invoke(*client, context);
  done = true;
  if (cancellation.joinable()) cancellation.join();
  return result;
}

CallResult ParseReply(const ModelConfig& model, const grpc::Status& status,
                      const std::string& bytes) {
  if (!status.ok()) return Failure(model, status);
  if (bytes.size() > model.max_response_bytes) {
    return {CallResult::Status::kProtocolError, 0, {},
            "model '" + model.id + "' gRPC response exceeds configured byte limit"};
  }
  try {
    return {CallResult::Status::kCompleted, 0, nlohmann::json::parse(bytes), {}};
  } catch (const std::exception& error) {
    return {CallResult::Status::kProtocolError, 0, {},
            "model '" + model.id + "' returned invalid JSON: " + error.what()};
  }
}
}  // namespace

CallResult GrpcCall(const ModelConfig& model, const nlohmann::json& request,
                    const CancelRequested& cancel_requested) {
  const std::string body = request.dump();
  if (body.size() > model.max_request_bytes) {
    return {CallResult::Status::kInvalidArgument, 0, {},
            "model '" + model.id + "' request exceeds configured byte limit"};
  }
  return Request(model, model.timeout, cancel_requested,
                 [&](protocol::ModelProviderService::Stub& client, grpc::ClientContext& context) {
                   protocol::CallRequest wire;
                   wire.set_model_id(model.provider_model_id.empty() ? model.id : model.provider_model_id);
                   wire.set_capability(std::string(CapabilityName(model.capability)));
                   wire.set_json(body);
                   protocol::CallResponse reply;
                   const grpc::Status status = client.Call(&context, wire, &reply);
                   return ParseReply(model, status, reply.json());
                 });
}

CallResult GrpcHealth(const ModelConfig& model, const CancelRequested& cancel_requested) {
  return Request(model, model.health_timeout, cancel_requested,
                 [&](protocol::ModelProviderService::Stub& client, grpc::ClientContext& context) {
                   protocol::HealthRequest wire;
                   wire.set_model_id(model.provider_model_id.empty() ? model.id : model.provider_model_id);
                   protocol::HealthResponse reply;
                   const grpc::Status status = client.Health(&context, wire, &reply);
                   return ParseReply(model, status, reply.json());
                 });
}

}  // namespace eos::runtime::models

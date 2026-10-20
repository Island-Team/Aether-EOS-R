#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"
#include "src/eos/runtime/models/vision_stream.h"

namespace eos::ext::models::detail {
namespace {
using runtime::models::CallResult;
using runtime::models::ModelConfig;

class GraspPlanSession final : public runtime::models::StreamSession {
 public:
  explicit GraspPlanSession(ModelConfig config) : config_(std::move(config)), stream_(config_) {}

  CallResult Call(const nlohmann::json& request,
                  const runtime::models::CancelRequested& cancelled) override {
    if (cancelled && cancelled()) return Cancelled(config_);
    try {
      const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
      const std::string payload = request.dump();
      if (payload.size() > config_.max_request_bytes)
        return {CallResult::Status::kInvalidArgument, 0, {}, "grasp plan request exceeds byte limit"};
      stream_.WriteText(payload, cancelled, deadline);
      auto response = stream_.ReadJson(cancelled, deadline);
      if (response.is_object() && response.contains("error") && response["error"].is_object()) {
        const auto& error = response["error"];
        const std::string code = error.value("code", "GRASP_PLAN_REJECTED");
        const std::string message = error.value("message", "grasp planner rejected request");
        return {CallResult::Status::kRemoteError, 0, {}, code + ": " + message};
      }
      if (response.is_object() && response.value("type", "") == "error")
        return {CallResult::Status::kRemoteError, 0, {},
                response.value("detail", "grasp planner rejected request")};
      return {CallResult::Status::kCompleted, 0, std::move(response), {}};
    } catch (const std::exception& error) {
      stream_.Close();
      return StreamException(config_, error);
    }
  }

  void Close() noexcept override { stream_.Close(); }

 private:
  ModelConfig config_;
  runtime::models::WebSocketClient stream_;
};

class GraspPlanAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const ModelConfig&, const nlohmann::json& request) const override {
    return request;
  }
  nlohmann::json DecodeResponse(const ModelConfig&, std::string_view,
                                std::string_view body) const override { return ParseJson(body); }
  void NormalizeResponse(const ModelConfig&, nlohmann::json*) const override {}
  std::unique_ptr<runtime::models::StreamSession> CreateStreamSession(
      const ModelConfig& config) const override {
    return std::make_unique<GraspPlanSession>(config);
  }
};
}  // namespace

std::shared_ptr<const runtime::models::Adapter> CreateGraspPlanAdapter() {
  return std::make_shared<GraspPlanAdapter>();
}
}  // namespace eos::ext::models::detail

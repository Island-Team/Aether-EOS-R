#include "src/eos/ext/models/adapter_factories.h"

#include "src/eos/ext/models/adapter_utils.h"
#include "src/eos/runtime/models/vision_stream.h"

namespace eos::ext::models::detail {
namespace {
using runtime::models::CallResult;
using runtime::models::ModelConfig;

nlohmann::json PrepareFrame(const nlohmann::json& request) {
  nlohmann::json payload{{"type", "frame"},
                         {"image_b64", request.at("input").at("data_b64")}};
  for (const char* key : {"conf", "iou"}) {
    if (request.contains(key)) payload[key] = request.at(key);
  }
  return payload;
}

void ValidateResult(const nlohmann::json& response) {
  if (!response.is_object()) throw std::runtime_error("litter stream response must be an object");
  if (response.value("type", "") == "error") {
    throw std::runtime_error(response.value("detail", "litter provider rejected frame"));
  }
  if (response.value("type", "") != "result" || !response.contains("objects") ||
      !response.at("objects").is_array()) {
    throw std::runtime_error("litter stream response is not a result");
  }
}

void NormalizeResult(nlohmann::json* response) {
  if (response == nullptr) return;
  ValidateResult(*response);
  nlohmann::json objects = std::move(response->at("objects"));
  *response = {{"objects", std::move(objects)}};
  (*response)["object_count"] = response->at("objects").size();
}

class Yolo11LitterSession final : public runtime::models::StreamSession {
 public:
  explicit Yolo11LitterSession(ModelConfig config)
      : config_(std::move(config)), stream_(config_) {}

  CallResult Call(const nlohmann::json& request,
                  const runtime::models::CancelRequested& cancel_requested) override {
    if (cancel_requested && cancel_requested()) return Cancelled(config_);
    try {
      const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
      const nlohmann::json provider = PrepareFrame(request);
      const std::string encoded = provider.dump();
      if (encoded.size() > config_.max_request_bytes) {
        return {CallResult::Status::kInvalidArgument, 0, {},
                "model '" + config_.id + "' request exceeds configured byte limit"};
      }
      stream_.WriteText(encoded, cancel_requested, deadline);
      nlohmann::json response = stream_.ReadJson(cancel_requested, deadline);
      if (response.value("type", "") == "error") {
        return {CallResult::Status::kRemoteError, 0, {},
                response.value("detail", "litter provider rejected frame")};
      }
      NormalizeResult(&response);
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

class Yolo11LitterAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const ModelConfig&, const nlohmann::json& request) const override {
    return PrepareFrame(request);
  }

  nlohmann::json DecodeResponse(const ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return ParseJson(body);
  }

  void NormalizeResponse(const ModelConfig&, nlohmann::json* response) const override {
    NormalizeResult(response);
  }

  void NormalizeHealthResponse(const ModelConfig& model,
                               nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) return;
    const std::string status = response->value("status", "starting");
    *response = {{"interface_version", "1.0"},
                 {"status", status == "ok" ? "ready" : "unavailable"},
                 {"service", {{"name", "island-yolo11-litter"}, {"version", "1.0"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {"vision.object_detection"}}};
  }

  std::unique_ptr<runtime::models::StreamSession> CreateStreamSession(
      const ModelConfig& model) const override {
    return std::make_unique<Yolo11LitterSession>(model);
  }
};
}  // namespace

std::shared_ptr<const runtime::models::Adapter> CreateYolo11LitterAdapter() {
  return std::make_shared<Yolo11LitterAdapter>();
}
}  // namespace eos::ext::models::detail

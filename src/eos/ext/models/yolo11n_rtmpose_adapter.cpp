#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/models/vision_stream.h"

namespace eos::ext::models::detail {
namespace {
using runtime::models::CallResult;
using runtime::models::Capability;
using runtime::models::ModelConfig;

class Yolo11nRtmposeSession final : public runtime::models::StreamSession {
 public:
  explicit Yolo11nRtmposeSession(ModelConfig config)
      : config_(std::move(config)), stream_(config_) {}

  CallResult Call(const nlohmann::json& request,
                  const runtime::models::CancelRequested& cancel_requested) override {
    const auto logger = eos::lib::GetLogger("models.adapter.rtmpose");
    if (cancel_requested && cancel_requested()) return Cancelled(config_);
    if (request.contains("iou")) {
      return {CallResult::Status::kInvalidArgument,
              0,
              {},
              "model '" + config_.id + "' WebSocket does not support iou"};
    }
    try {
      const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
      nlohmann::json provider{{"type", "frame"}, {"image_b64", request.at("input").at("data_b64")}};
      if (request.contains("depth_input")) {
        provider["depth_b64"] = request.at("depth_input").at("data_b64");
        provider["depth_scale"] = request.value("depth_scale", 0.001);
        if (request.contains("depth_intrinsics"))
          provider["depth_intrinsics"] = request.at("depth_intrinsics");
      }
      for (const char* key : {"conf", "kpt_conf"}) {
        if (request.contains(key)) provider[key] = request.at(key);
      }
      for (const char* key : {"persons", "furniture"})
        if (request.contains(key)) provider[key] = request.at(key);
      const std::string encoded = provider.dump();
      if (encoded.size() > config_.max_request_bytes) {
        return {CallResult::Status::kInvalidArgument,
                0,
                {},
                "model '" + config_.id + "' request exceeds configured byte limit"};
      }
      eos::lib::LogEvent(logger, spdlog::level::trace, "rtmpose_stream_request_prepared",
                         {{"model_id", config_.id},
                          {"capability", std::string(runtime::models::CapabilityName(
                                             config_.capability))},
                          {"request_bytes", encoded.size()},
                          {"depth_available", request.contains("depth_input")},
                          {"confidence_override", request.contains("conf")},
                          {"keypoint_confidence_override", request.contains("kpt_conf")}});
      stream_.WriteText(encoded, cancel_requested, deadline);
      nlohmann::json response = stream_.ReadJson(cancel_requested, deadline);
      if (logger->should_log(spdlog::level::trace)) {
        eos::lib::LogEvent(logger, spdlog::level::trace, "rtmpose_stream_response_received",
                           {{"model_id", config_.id},
                            {"response_type", response.is_object() ? response.value("type", "")
                                                                    : "invalid"},
                            {"response_bytes", response.dump().size()}});
      }
      if (!response.is_object()) {
        return {
            CallResult::Status::kProtocolError, 0, {}, "pose stream response must be an object"};
      }
      if (response.value("type", "") == "error") {
        return {CallResult::Status::kRemoteError,
                0,
                {},
                response.value("detail", "pose provider rejected frame")};
      }
      if (response.value("type", "") != "result" || !response.contains("persons") ||
          !response.at("persons").is_array()) {
        return {CallResult::Status::kProtocolError, 0, {}, "pose stream response is not a result"};
      }
      try {
        NormalizeVisionPeople(config_.capability, &response);
      } catch (const std::invalid_argument& error) {
        return {CallResult::Status::kProtocolError,
                0,
                {},
                "model '" + config_.id + "' returned invalid pose data: " + error.what()};
      }
      if (logger->should_log(spdlog::level::debug)) {
        nlohmann::json classifications = nlohmann::json::array();
        for (const auto& person : response.at("persons")) {
          if (!person.is_object()) continue;
          nlohmann::json classification = nlohmann::json::object();
          for (const char* key : {"track_id", "pose", "action", "posture", "posture_state",
                                  "behavior_event"}) {
            if (person.contains(key)) classification[key] = person.at(key);
          }
          classifications.push_back(std::move(classification));
        }
        eos::lib::LogEvent(
            logger, spdlog::level::debug, "rtmpose_stream_response_normalized",
            {{"model_id", config_.id},
             {"capability", std::string(runtime::models::CapabilityName(config_.capability))},
             {"result_count", response.value("person_count", 0)},
             {"classifications", std::move(classifications)}});
      }
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

class Yolo11nRtmposeAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig&,
                                const nlohmann::json& request) const override {
    nlohmann::json payload{{"type", "frame"}, {"image_b64", request.at("input").at("data_b64")}};
    const auto depth = request.find("depth_input");
    if (depth != request.end()) {
      payload["depth_b64"] = depth->at("data_b64");
      if (request.contains("depth_scale")) payload["depth_scale"] = request.at("depth_scale");
      if (request.contains("depth_intrinsics"))
        payload["depth_intrinsics"] = request.at("depth_intrinsics");
    }
    for (const char* key : {"conf", "kpt_conf"})
      if (request.contains(key)) payload[key] = request.at(key);
    for (const char* key : {"persons", "furniture"})
      if (request.contains(key)) payload[key] = request.at(key);
    return payload;
  }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return ParseJson(body);
  }
  void NormalizeResponse(const runtime::models::ModelConfig& model,
                          nlohmann::json* response) const override {
    NormalizeVisionPeople(model.capability, response);
  }
  void NormalizeHealthResponse(const runtime::models::ModelConfig& model,
                               nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) return;
    const std::string status = response->value("status", "starting");
    *response = {{"interface_version", "1.0"},
                 {"status", status == "ok" ? "ready" : "unavailable"},
                 {"service", {{"name", "island-yolo11n-rtmpose"}, {"version", "1.0"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {std::string(runtime::models::CapabilityName(model.capability))}}};
  }
  std::unique_ptr<runtime::models::StreamSession> CreateStreamSession(
      const runtime::models::ModelConfig& model) const override {
    return std::make_unique<Yolo11nRtmposeSession>(model);
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateYolo11nRtmposeAdapter() {
  return std::make_shared<Yolo11nRtmposeAdapter>();
}
}  // namespace eos::ext::models::detail

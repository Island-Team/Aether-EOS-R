#include <algorithm>
#include <cctype>

#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/models/vision_stream.h"

namespace eos::ext::models::detail {
namespace {
using runtime::models::CallResult;
using runtime::models::ModelConfig;
using runtime::models::WebSocketClient;

bool KnownIdentity(const std::string& name) {
  std::string lowered = name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return !lowered.empty() && lowered != "unknown";
}

void Start(WebSocketClient* stream, const ModelConfig& config,
           const runtime::models::CancelRequested& cancel_requested,
           WebSocketClient::Deadline deadline) {
  const auto timeout_ms = std::clamp<std::int64_t>(config.timeout.count() * 4 / 5, 1000, 60000);
  stream->WriteText(
      nlohmann::json(
          {{"max_fps", 30.0}, {"register_timeout_ms", timeout_ms}, {"max_register_frames", 1}})
          .dump(),
      cancel_requested, deadline);
  const nlohmann::json ready = stream->ReadJson(cancel_requested, deadline);
  if (!ready.is_object() || ready.value("event", "") != "ready") {
    throw std::runtime_error("ArcFace stream did not send ready");
  }
}

CallResult NormalizeRecognition(const nlohmann::json& response) {
  if (!response.is_object() || response.value("event", "") != "frame_result" ||
      !response.contains("faces") || !response.at("faces").is_array()) {
    return {
        CallResult::Status::kProtocolError, 0, {}, "ArcFace stream response is not a frame_result"};
  }
  nlohmann::json faces = nlohmann::json::array();
  for (const nlohmann::json& face : response.at("faces")) {
    if (!face.is_object() || !face.contains("bbox")) continue;
    const std::string name = face.value("name", "Unknown");
    const bool known = KnownIdentity(name);
    nlohmann::json normalized{{"identity", known ? name : "unknown"},
                              {"known", known},
                              {"similarity", std::clamp(face.value("score", 0.0), 0.0, 1.0)},
                              {"bbox", face.at("bbox")}};
    if (face.contains("direction_px") && face.at("direction_px").is_number()) {
      normalized["direction_px"] = face.at("direction_px");
    }
    faces.push_back(std::move(normalized));
  }
  nlohmann::json data{{"faces", std::move(faces)}};
  data["face_count"] = data.at("faces").size();
  return {CallResult::Status::kCompleted,
          0,
          {{"status", "success"}, {"operation", "recognize"}, {"data", std::move(data)}},
          {}};
}

class ArcFaceSession final : public runtime::models::StreamSession {
 public:
  explicit ArcFaceSession(ModelConfig config) : config_(std::move(config)), stream_(config_) {}

  CallResult Call(const nlohmann::json& request,
                  const runtime::models::CancelRequested& cancel_requested) override {
    if (cancel_requested && cancel_requested()) return Cancelled(config_);
    const std::string operation = request.value("operation", "");
    if (operation != "recognize" && operation != "register") {
      return {CallResult::Status::kInvalidArgument,
              0,
              {},
              "model '" + config_.id + "' WebSocket supports recognize and register operations"};
    }
    if (operation == "recognize" && request.value("return_image", false)) {
      return {CallResult::Status::kInvalidArgument,
              0,
              {},
              "model '" + config_.id + "' WebSocket does not support return_image"};
    }
    try {
      const std::vector<std::uint8_t> image =
          DecodeBase64(request.at("input").at("data_b64").get<std::string>());
      if (image.size() > config_.max_request_bytes) {
        return {CallResult::Status::kInvalidArgument,
                0,
                {},
                "model '" + config_.id + "' request exceeds configured byte limit"};
      }
      return operation == "recognize"
                 ? Recognize(image, cancel_requested)
                 : Register(image, request.at("name").get<std::string>(), cancel_requested);
    } catch (const std::invalid_argument& error) {
      return {CallResult::Status::kInvalidArgument, 0, {}, error.what()};
    } catch (const std::exception& error) {
      Close();
      return StreamException(config_, error);
    }
  }

  void Close() noexcept override {
    stream_.Close();
    ready_ = false;
  }

 private:
  CallResult Recognize(const std::vector<std::uint8_t>& image,
                       const runtime::models::CancelRequested& cancel_requested) {
    const auto logger = eos::lib::GetLogger("models.adapter.arcface");
    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
    if (!ready_) {
      stream_.Connect(cancel_requested, deadline);
      Start(&stream_, config_, cancel_requested, deadline);
      ready_ = true;
      eos::lib::LogEvent(logger, spdlog::level::debug, "arcface_stream_ready",
                         {{"model_id", config_.id}});
    }
    eos::lib::LogEvent(logger, spdlog::level::trace, "arcface_recognition_request_started",
                       {{"model_id", config_.id}, {"image_bytes", image.size()}});
    stream_.WriteBinary(image, cancel_requested, deadline);
    while (true) {
      const nlohmann::json event = stream_.ReadJson(cancel_requested, deadline);
      const std::string type = event.value("event", "");
      eos::lib::LogEvent(logger, spdlog::level::trace, "arcface_stream_event_received",
                         {{"model_id", config_.id},
                          {"event_type", type},
                          {"response_bytes", event.dump().size()}});
      if (type == "frame_result") {
        CallResult result = NormalizeRecognition(event);
        eos::lib::LogEvent(
            logger,
            result.status == CallResult::Status::kCompleted ? spdlog::level::debug
                                                            : spdlog::level::warn,
            "arcface_recognition_response_normalized",
            {{"model_id", config_.id},
             {"status", static_cast<int>(result.status)},
             {"face_count", result.status == CallResult::Status::kCompleted
                                  ? result.response.at("data").value("face_count", 0)
                                  : 0}});
        return result;
      }
      if (type == "warning") {
        eos::lib::LogEvent(logger, spdlog::level::warn, "arcface_recognition_warning",
                           {{"model_id", config_.id},
                            {"reason", event.value("message", "ArcFace rejected frame")}});
        return {CallResult::Status::kRemoteError,
                0,
                {},
                event.value("message", "ArcFace rejected frame")};
      }
    }
  }

  CallResult Register(const std::vector<std::uint8_t>& image, const std::string& name,
                      const runtime::models::CancelRequested& cancel_requested) {
    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
    WebSocketClient registration(config_);
    registration.Connect(cancel_requested, deadline);
    Start(&registration, config_, cancel_requested, deadline);
    registration.WriteText(
        nlohmann::json({{"event", "register_start"}, {"name", name}, {"frames", 1}}).dump(),
        cancel_requested, deadline);
    if (registration.ReadJson(cancel_requested, deadline).value("event", "") !=
        "register_started") {
      return {CallResult::Status::kProtocolError, 0, {}, "ArcFace registration did not start"};
    }
    registration.WriteBinary(image, cancel_requested, deadline);
    while (true) {
      const nlohmann::json event = registration.ReadJson(cancel_requested, deadline);
      const std::string type = event.value("event", "");
      if (type == "registered") {
        const std::string registered_name = event.value("name", "");
        if (registered_name.empty()) {
          return {CallResult::Status::kProtocolError,
                  0,
                  {},
                  "ArcFace registered event does not contain a name"};
        }
        return {CallResult::Status::kCompleted,
                0,
                {{"status", "success"},
                 {"operation", "register"},
                 {"data", {{"name", registered_name}}}},
                {}};
      }
      if (type == "register_failed" || type == "warning") {
        return {CallResult::Status::kRemoteError,
                0,
                {},
                type == "register_failed"
                    ? "ArcFace registration failed: " + event.value("reason", "unknown")
                    : event.value("message", "ArcFace registration warning")};
      }
    }
  }

  ModelConfig config_;
  WebSocketClient stream_;
  bool ready_{false};
};

class ArcFaceAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig&,
                                const nlohmann::json& request) const override {
    return request;
  }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return ParseJson(body);
  }
  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json*) const override {}
  void NormalizeHealthResponse(const runtime::models::ModelConfig& model,
                               nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) return;
    const std::string status = response->value("status", "starting");
    const std::string provider_model =
        response->value("model", model.provider_model_id.empty() ? std::string("arcface")
                                                                 : model.provider_model_id);
    *response = {
        {"interface_version", "1.0"},
        {"status", status == "ok" ? "ready" : "unavailable"},
        {"service", {{"name", "island-arcface"}, {"version", "1.0"}}},
        {"model", {{"id", provider_model}, {"version", response->value("model_name", "unknown")}}},
        {"capabilities", {"vision.face"}}};
  }
  std::unique_ptr<runtime::models::StreamSession> CreateStreamSession(
      const runtime::models::ModelConfig& model) const override {
    return std::make_unique<ArcFaceSession>(model);
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateArcFaceAdapter() {
  return std::make_shared<ArcFaceAdapter>();
}
}  // namespace eos::ext::models::detail

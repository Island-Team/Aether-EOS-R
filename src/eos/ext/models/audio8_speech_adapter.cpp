#include "src/eos/ext/models/adapter_factories.h"

#include <stdexcept>

#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class Audio8SpeechAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                const nlohmann::json& request) const override {
    nlohmann::json payload{{"input", request.at("text")}, {"model", model.provider_model_id},
                           {"response_format", "wav"}, {"stream", request.value("stream", false)}};
    Copy(request, "temperature", "temperature", &payload);
    Copy(request, "top_p", "top_p", &payload);
    Copy(request, "reference_audio_b64", "ref_audio", &payload);
    Copy(request, "reference_audio_media_type", "ref_audio_media_type", &payload);
    Copy(request, "prompt_text", "ref_text", &payload);
    return payload;
  }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view media_type,
                                std::string_view body) const override {
    const std::size_t parameter = media_type.find(';');
    if (parameter != std::string_view::npos) media_type = media_type.substr(0, parameter);
    if (media_type != "audio/wav" && media_type != "audio/wave" && media_type != "audio/x-wav") {
      throw std::runtime_error("expected WAV audio, received content type '" +
                               std::string(media_type) + "'");
    }
    return {{"data_b64", EncodeBase64(body)}, {"media_type", "audio/wav"}, {"format", "wav"}};
  }
  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json*) const override {}
  void NormalizeHealthResponse(const runtime::models::ModelConfig& model,
                                nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) return;
    const std::string status = response->value("status", "starting");
    const bool ready = status == "ok" ||
                       (status == "healthy" && response->value("running", true));
    const std::string message =
        ready ? std::string() : response->value("message", "provider status is '" + status + "'");
    *response = {{"interface_version", "1.0"},
                 {"status", ready ? "ready" : "unavailable"},
                 {"service", {{"name", "island-audio8-tts"}, {"version", "1.0"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {"audio.synthesis"}}};
    if (!message.empty()) (*response)["message"] = message;
  }

 private:
  static void Copy(const nlohmann::json& source, const char* source_key, const char* target_key,
                   nlohmann::json* target) {
    const auto found = source.find(source_key);
    if (found != source.end()) (*target)[target_key] = *found;
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateAudio8SpeechAdapter() {
  return std::make_shared<Audio8SpeechAdapter>();
}
}  // namespace eos::ext::models::detail

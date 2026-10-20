#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class SensevoiceVadAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig&,
                                const nlohmann::json& request) const override {
    nlohmann::json input = request.at("input");
    if (!input.contains("filename")) input["filename"] = "audio.wav";
    if (!input.contains("media_type")) input["media_type"] = "audio/wav";
    if (!input.contains("sample_rate_hz")) input["sample_rate_hz"] = 16000;
    if (!input.contains("channels")) input["channels"] = 1;
    nlohmann::json payload{{"input", std::move(input)}};
    if (request.contains("threshold")) payload["threshold"] = request.at("threshold");
    return payload;
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
    *response = {{"interface_version", "1.0"},
                 {"status", status == "ok" ? "ready" : "unavailable"},
                 {"service", {{"name", "island-sensevoice-vad"}, {"version", "1.0"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {"audio.vad"}}};
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateSensevoiceVadAdapter() {
  return std::make_shared<SensevoiceVadAdapter>();
}
}  // namespace eos::ext::models::detail

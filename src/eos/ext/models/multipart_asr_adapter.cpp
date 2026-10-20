#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class Qwen3AsrAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                 const nlohmann::json& request) const override {
    nlohmann::json payload{{"input", request.at("input")},
                           {"response_format", "json"}};
    if (request.contains("language")) payload["language"] = request.at("language");
    if (request.contains("prompt")) payload["prompt"] = request.at("prompt");
    if (request.contains("temperature")) payload["temperature"] = request.at("temperature");
    return payload;
  }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override { return ParseJson(body); }
  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json*) const override {}
  void NormalizeHealthResponse(const runtime::models::ModelConfig& model,
                               nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) return;
    const std::string status = response->value("status", "starting");
    *response = {{"interface_version", "1.0"},
                 {"status", status == "ok" ? "ready" : "unavailable"},
                 {"service", {{"name", "island-qwen3-asr"}, {"version", "1.0"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {"audio.transcription"}}};
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateQwen3AsrAdapter() {
  return std::make_shared<Qwen3AsrAdapter>();
}
}  // namespace eos::ext::models::detail

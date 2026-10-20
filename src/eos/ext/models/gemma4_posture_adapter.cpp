#include "src/eos/ext/models/adapter_factories.h"

#include <stdexcept>

namespace eos::ext::models::detail {
namespace {
class Gemma4PostureAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                const nlohmann::json& request) const override {
    const auto& input = request.at("input");
    const auto& bbox = request.at("bbox");
    if (!bbox.is_array() || bbox.size() != 4 ||
        !bbox.at(0).is_number() || !bbox.at(1).is_number() ||
        !bbox.at(2).is_number() || !bbox.at(3).is_number() ||
        bbox.at(2).get<double>() <= bbox.at(0).get<double>() ||
        bbox.at(3).get<double>() <= bbox.at(1).get<double>()) {
      throw std::invalid_argument("target bbox must have positive width and height");
    }
    const std::string image_url = "data:image/jpeg;base64," + input.at("data_b64").get<std::string>();
    const std::string prompt =
        "Classify only the person inside bounding box " + bbox.dump() +
        " (pixel coordinates x1,y1,x2,y2). Return only a JSON object with two fields: "
        "posture_state (standing, sitting, sitting_on_furniture, lying, lying_on_furniture, "
        "lying_on_floor, or unknown) and action (waving, fallen, none, or unknown). "
        "The two labels must describe the same person and moment. Use fallen only with a "
        "lying posture. Use unknown when visual evidence is insufficient.";
    return {{"model", model.provider_model_id},
            {"messages", {{{"role", "user"},
                           {"content", {{{"type", "text"}, {"text", prompt}},
                                        {{"type", "image_url"}, {"image_url", {{"url", image_url}}}}}}}}},
            {"temperature", 0},
            {"max_tokens", 96},
            {"stream", false},
            {"chat_template_kwargs", {{"enable_thinking", false}}},
            {"response_format", {{"type", "json_object"}}}};
  }

  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override {
    const auto envelope = nlohmann::json::parse(body);
    const auto& choices = envelope.at("choices");
    if (!choices.is_array() || choices.size() != 1) throw std::invalid_argument("expected one choice");
    const auto& content = choices.at(0).at("message").at("content");
    if (!content.is_string()) throw std::invalid_argument("expected text content");
    return nlohmann::json::parse(content.get<std::string>());
  }

  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json* response) const override {
    if (!response || !response->is_object() || response->size() != 2 ||
        !response->contains("posture_state") || !response->at("posture_state").is_string() ||
        !response->contains("action") || !response->at("action").is_string()) {
      throw std::invalid_argument("expected posture_state and action strings");
    }
    const std::string value = response->at("posture_state").get<std::string>();
    if (value != "standing" && value != "sitting" && value != "sitting_on_furniture" &&
        value != "lying" && value != "lying_on_furniture" &&
        value != "lying_on_floor" && value != "unknown") {
      throw std::invalid_argument("unsupported posture_state");
    }
    const std::string action = response->at("action").get<std::string>();
    if (action != "waving" && action != "fallen" && action != "none" && action != "unknown")
      throw std::invalid_argument("unsupported action");
    if (action == "fallen" && value != "lying" && value != "lying_on_furniture" &&
        value != "lying_on_floor")
      throw std::invalid_argument("fallen action requires lying posture");
  }
};
}  // namespace

std::shared_ptr<const runtime::models::Adapter> CreateGemma4PostureAdapter() {
  return std::make_shared<Gemma4PostureAdapter>();
}
}  // namespace eos::ext::models::detail

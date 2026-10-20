#include "src/eos/ext/models/adapter_factories.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace eos::ext::models::detail {
namespace {
class OpenAiVlmAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                const nlohmann::json& request) const override {
    if (request.contains("bbox")) {
      return CreateGemma4PostureAdapter()->PrepareRequest(model, request);
    }
    nlohmann::json messages = request.at("messages");
    if (request.contains("input")) {
      // An image belongs to the latest user turn, never to a system or assistant message.
      auto user = messages.end();
      for (auto it = messages.begin(); it != messages.end(); ++it) {
        if (it->at("role") == "user") user = it;
      }
      if (user == messages.end()) {
        throw std::invalid_argument("VLM image requires a user message");
      }
      const std::string content = user->at("content").get<std::string>();
      const std::string image_url = "data:image/jpeg;base64," +
                                    request.at("input").at("data_b64").get<std::string>();
      (*user)["content"] = {{{"type", "text"}, {"text", content}},
                             {{"type", "image_url"}, {"image_url", {{"url", image_url}}}}};
    }
    nlohmann::json payload{{"model", model.provider_model_id},
                           {"messages", std::move(messages)},
                           {"chat_template_kwargs", {{"enable_thinking", request.value("reasoning", false)}}},
                           {"stream", false}};
    for (const char* field : {"temperature", "max_tokens"}) {
      if (request.contains(field)) payload[field] = request.at(field);
    }
    return payload;
  }

  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return nlohmann::json::parse(body);
  }

  void NormalizeResponse(const runtime::models::ModelConfig&,
                         nlohmann::json* response) const override {
    if (!response || !response->is_object()) {
      throw std::runtime_error("OpenAI VLM response must be an object");
    }
    try {
      const auto& content = response->at("choices").at(0).at("message").at("content");
      if (!content.is_string()) throw std::runtime_error("OpenAI VLM response must contain text");
      const auto& value = content.get_ref<const std::string&>();
      if (value.empty() || std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
          })) throw std::runtime_error("OpenAI VLM response must contain text");
      *response = {{"text", content}};
    } catch (const nlohmann::json::exception&) {
      throw std::runtime_error("OpenAI VLM response is missing choices[0].message.content");
    }
  }

  void NormalizeResponseForRequest(const runtime::models::ModelConfig& model,
                                   const nlohmann::json& request,
                                   nlohmann::json* response) const override {
    if (!request.contains("bbox")) {
      NormalizeResponse(model, response);
      return;
    }
    const auto& content = response->at("choices").at(0).at("message").at("content");
    if (!content.is_string()) throw std::invalid_argument("expected posture JSON content");
    *response = nlohmann::json::parse(content.get<std::string>());
    CreateGemma4PostureAdapter()->NormalizeResponse(model, response);
  }

  void NormalizeHealthResponse(const runtime::models::ModelConfig& model,
                               nlohmann::json* response) const override {
    if (*response != nlohmann::json{{"status", "ok"}}) return;
    *response = {{"interface_version", "1.0"},
                 {"status", "ready"},
                 {"service", {{"name", "gemma-vllm"}, {"version", "unknown"}}},
                 {"model", {{"id", model.provider_model_id}, {"version", "unknown"}}},
                 {"capabilities", {"vision.vlm", "vision.posture_classification"}}};
  }
};
}  // namespace

std::shared_ptr<const runtime::models::Adapter> CreateOpenAiVlmAdapter() {
  return std::make_shared<OpenAiVlmAdapter>();
}
}  // namespace eos::ext::models::detail

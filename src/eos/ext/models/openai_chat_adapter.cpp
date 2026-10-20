#include "src/eos/ext/models/adapter_factories.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class OpenAiChatAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                const nlohmann::json& request) const override {
    nlohmann::json payload{{"model", model.provider_model_id},
                           {"messages", request.at("messages")},
                           {"chat_template_kwargs", {{"enable_thinking", false}}},
                           {"stream", false}};
    Copy(request, "temperature", &payload);
    Copy(request, "max_tokens", &payload);
    return payload;
  }

  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return ParseJson(body);
  }

  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json* response) const override {
    if (response == nullptr || !response->is_object()) {
      throw std::runtime_error("OpenAI chat response must be an object");
    }
    try {
      const auto& content = response->at("choices").at(0).at("message").at("content");
      if (!content.is_string()) {
        throw std::runtime_error("OpenAI chat response content must be a non-whitespace string");
      }
      const auto& text = content.get_ref<const std::string&>();
      if (text.empty() || std::all_of(text.begin(), text.end(), [](unsigned char character) {
            return std::isspace(character) != 0;
          })) {
        throw std::runtime_error("OpenAI chat response content must be a non-whitespace string");
      }
      *response = {{"text", content}};
    } catch (const nlohmann::json::exception&) {
      throw std::runtime_error("OpenAI chat response is missing choices[0].message.content");
    }
  }

 private:
  static void Copy(const nlohmann::json& source, const char* key, nlohmann::json* target) {
    const auto found = source.find(key);
    if (found != source.end()) (*target)[key] = *found;
  }
};
}  // namespace

std::shared_ptr<const runtime::models::Adapter> CreateOpenAiChatAdapter() {
  return std::make_shared<OpenAiChatAdapter>();
}
}  // namespace eos::ext::models::detail

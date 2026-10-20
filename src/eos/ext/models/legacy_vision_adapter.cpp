#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class LegacyVisionAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig& model,
                                const nlohmann::json& request) const override {
    nlohmann::json payload = request;
    if (!model.provider_model_id.empty()) payload["model"] = model.provider_model_id;
    return payload;
  }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override { return ParseJson(body); }
  void NormalizeResponse(const runtime::models::ModelConfig& model,
                         nlohmann::json* response) const override {
    NormalizeVisionPeople(model.capability, response);
  }
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateLegacyVisionAdapter() {
  return std::make_shared<LegacyVisionAdapter>();
}
}  // namespace eos::ext::models::detail

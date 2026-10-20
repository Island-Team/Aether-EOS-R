#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"

namespace eos::ext::models::detail {
namespace {
class CanonicalAdapter final : public runtime::models::Adapter {
 public:
  nlohmann::json PrepareRequest(const runtime::models::ModelConfig&,
                                const nlohmann::json& request) const override { return request; }
  nlohmann::json DecodeResponse(const runtime::models::ModelConfig&, std::string_view,
                                std::string_view body) const override { return ParseJson(body); }
  void NormalizeResponse(const runtime::models::ModelConfig&, nlohmann::json*) const override {}
};
}  // namespace
std::shared_ptr<const runtime::models::Adapter> CreateCanonicalAdapter() {
  return std::make_shared<CanonicalAdapter>();
}
}  // namespace eos::ext::models::detail

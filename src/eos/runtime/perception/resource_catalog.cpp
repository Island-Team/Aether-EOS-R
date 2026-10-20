#include "src/eos/runtime/perception/resource_catalog.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

#include "src/eos/runtime/perception/body/reader.h"

namespace eos::runtime::perception {
namespace {

bool MatchesTemplate(const std::string& uri_template, const std::string& uri) {
  std::size_t template_start = 0;
  std::size_t uri_start = 0;
  for (;;) {
    const std::size_t template_end = uri_template.find('/', template_start);
    const std::size_t uri_end = uri.find('/', uri_start);
    const std::string template_segment = uri_template.substr(
        template_start,
        template_end == std::string::npos ? template_end : template_end - template_start);
    const std::string uri_segment =
        uri.substr(uri_start, uri_end == std::string::npos ? uri_end : uri_end - uri_start);
    const bool placeholder = template_segment.size() > 2 && template_segment.front() == '{' &&
                             template_segment.back() == '}';
    if ((!placeholder && template_segment != uri_segment) || uri_segment.empty()) return false;
    if (template_end == std::string::npos || uri_end == std::string::npos)
      return template_end == uri_end;
    template_start = template_end + 1;
    uri_start = uri_end + 1;
  }
}

}  // namespace

void ResourceCatalog::Register(ProductHistoryReader reader) {
  const std::string uri = reader.declaration().uri;
  if (uri.empty()) throw std::invalid_argument("resource history URI is required");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!histories_.emplace(uri, std::move(reader)).second)
    throw std::invalid_argument("resource history already registered: " + uri);
}

void ResourceCatalog::Register(ProductHistoryReader reader, Demand demand,
                               std::chrono::milliseconds publication_timeout) {
  if (!demand) throw std::invalid_argument("resource demand callback is required");
  if (publication_timeout <= std::chrono::milliseconds::zero())
    throw std::invalid_argument("resource publication timeout must be positive");
  const std::string uri = reader.declaration().uri;
  if (uri.empty()) throw std::invalid_argument("resource history URI is required");
  std::lock_guard<std::mutex> lock(mutex_);
  if (!histories_.emplace(uri, std::move(reader)).second)
    throw std::invalid_argument("resource history already registered: " + uri);
  demands_.emplace(uri, std::move(demand));
  publication_timeouts_.emplace(uri, publication_timeout);
}

void ResourceCatalog::RegisterProvider(Provider provider) {
  if (!provider.list || !provider.read || !provider.supports_template)
    throw std::invalid_argument("resource provider callbacks are required");
  std::lock_guard<std::mutex> lock(mutex_);
  providers_.push_back(std::move(provider));
}

std::optional<ProducerLease> ResourceCatalog::AcquireDemand(const std::string& uri) const {
  Demand demand;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = demands_.find(uri);
    if (found == demands_.end()) return std::nullopt;
    demand = found->second;
  }
  return demand();
}

std::optional<ProductHistoryReader> ResourceCatalog::FindHistory(const std::string& uri) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = histories_.find(uri);
  if (found == histories_.end()) return std::nullopt;
  return found->second;
}

nlohmann::json ResourceCatalog::ListResources() const {
  nlohmann::json result = legacy_ == nullptr ? nlohmann::json::array() : legacy_->ListResources();
  std::vector<ProductHistoryReader> histories;
  std::vector<Provider> providers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [uri, reader] : histories_) histories.push_back(reader);
    providers = providers_;
  }
  for (const auto& reader : histories) {
    const auto& declaration = reader.declaration();
    result.erase(std::remove_if(result.begin(), result.end(),
                                [&](const auto& item) {
                                  return item.is_object() &&
                                         item.value("uri", "") == declaration.uri;
                                }),
                 result.end());
    result.push_back(
        {{"uri", declaration.uri}, {"schema_uri", declaration.schema_uri}, {"history", true}});
  }
  for (const auto& provider : providers) {
    const nlohmann::json listed = provider.list();
    if (!listed.is_array()) throw std::runtime_error("resource provider list must be an array");
    for (const auto& item : listed) {
      const std::string uri = item.value("uri", "");
      if (uri.empty()) throw std::runtime_error("resource provider returned an empty URI");
      result.erase(std::remove_if(result.begin(), result.end(),
                                  [&](const auto& existing) {
                                    return existing.is_object() && existing.value("uri", "") == uri;
                                  }),
                   result.end());
      result.push_back(item);
    }
  }
  return result;
}

nlohmann::json ResourceCatalog::ReadResource(const std::string& uri, bool log_success) const {
  if (const auto reader = FindHistory(uri)) {
    const ProductLookup baseline = reader->Latest();
    auto demand = AcquireDemand(uri);
    ProductPtr product;
    if (demand && (!baseline.product || demand->activated())) {
      std::chrono::milliseconds timeout;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        timeout = publication_timeouts_.at(uri);
      }
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      const ProductWait next =
          reader->WaitNewer(baseline.product ? baseline.product->sequence : 0,
                            [&] { return std::chrono::steady_clock::now() >= deadline; });
      product = next.status == WaitStatus::kProduct ? next.product : nullptr;
    } else {
      product = reader->Latest().product;
    }
    if (!product) throw std::runtime_error("resource has no published product: " + uri);
    return product->value;
  }
  std::vector<Provider> providers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    providers = providers_;
  }
  for (const auto& provider : providers)
    if (auto value = provider.read(uri)) return std::move(*value);
  if (legacy_ == nullptr) throw std::runtime_error("unknown resource uri: " + uri);
  return legacy_->BuildByUri(uri, log_success);
}

bool ResourceCatalog::SupportsResourceTemplate(const std::string& uri_template) const {
  std::vector<Provider> providers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : histories_) {
      if (MatchesTemplate(uri_template, entry.first)) return true;
    }
    providers = providers_;
  }
  for (const auto& provider : providers)
    if (provider.supports_template(uri_template)) return true;
  return legacy_ != nullptr && legacy_->SupportsResourceTemplate(uri_template);
}

}  // namespace eos::runtime::perception

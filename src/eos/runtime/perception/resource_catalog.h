#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

#include "src/eos/runtime/perception/producer_coordinator.h"
#include "src/eos/runtime/perception/product_history.h"

namespace eos::runtime::perception::body {
class Reader;
}

namespace eos::runtime::perception {

/// Read-only catalog for published resources. Registered publication histories
/// take precedence over the legacy body projection for the same URI.
class ResourceCatalog final {
 public:
  explicit ResourceCatalog(body::Reader* legacy = nullptr) : legacy_(legacy) {}

  using Demand = std::function<ProducerLease()>;
  struct Provider {
    std::function<nlohmann::json()> list;
    std::function<std::optional<nlohmann::json>(const std::string&)> read;
    std::function<bool(const std::string&)> supports_template;
  };

  void Register(ProductHistoryReader reader);
  void Register(ProductHistoryReader reader, Demand demand,
                std::chrono::milliseconds publication_timeout);
  void RegisterProvider(Provider provider);
  nlohmann::json ListResources() const;
  nlohmann::json ReadResource(const std::string& uri, bool log_success = true) const;
  std::optional<ProductHistoryReader> FindHistory(const std::string& uri) const;
  /// Holds producer demand until the returned lease is destroyed.
  std::optional<ProducerLease> AcquireDemand(const std::string& uri) const;
  bool SupportsResourceTemplate(const std::string& uri_template) const;

 private:
  body::Reader* legacy_;
  mutable std::mutex mutex_;
  std::map<std::string, ProductHistoryReader> histories_;
  std::map<std::string, Demand> demands_;
  std::map<std::string, std::chrono::milliseconds> publication_timeouts_;
  std::vector<Provider> providers_;
};

}  // namespace eos::runtime::perception

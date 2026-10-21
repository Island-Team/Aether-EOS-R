#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <vector>

#include "binding.h"

namespace eos::runtime::hardware_runtime {

/// Opaque, validated adapter configuration. Concrete settings live with the adapter.
struct BackendSettings {
  virtual ~BackendSettings() = default;
  virtual std::uint64_t CameraFrameLimitBytes() const { return 4 * 1024 * 1024; }
};

/// One statically linked adapter's configuration and construction hooks.
struct BackendDescriptor {
  std::string type;
  bool compiled{true};
  std::function<bool(std::string_view)> supports_class;
  std::function<std::shared_ptr<const BackendSettings>(const toml::table&, const PartConfig&,
                                                       const std::string&, const std::string&)>
      parse;
  std::function<void(PartConfig&, const toml::table&)> finalize_part;
  std::function<bool(const PrimitiveSpec&)> supports_primitive;
  std::function<void(const DeviceBinding&, const std::string&, const std::string&)> validate_device;
  std::function<void(const Binding&, const std::string&, const std::string&)> validate_binding;
  std::function<std::unique_ptr<hardware::Backend>(std::string, const PartConfig&,
                                                   const BackendSettings&)>
      create;
};

class BackendCatalog {
 public:
  void Register(BackendDescriptor descriptor);
  const BackendDescriptor* Find(std::string_view type) const;

 private:
  std::vector<BackendDescriptor> descriptors_;
};

}  // namespace eos::runtime::hardware_runtime

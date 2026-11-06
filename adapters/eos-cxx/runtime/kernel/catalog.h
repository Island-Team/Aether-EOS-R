#pragma once

#include <string>
#include <vector>

#include "adapters/eos-cxx/runtime/hardware/manager/capability_bindings.h"

namespace eos::runtime::kernel {

// An immutable view of capabilities admitted for this runtime. Each admitted
// hardware primitive has a stable canonical name; configured slots may add
// human-facing aliases. The descriptor seam can later host software owners
// without changing the broker's caller contract.
class Catalog {
 public:
  Catalog(const hardware_runtime::Registry& registry,
          const std::vector<hardware_runtime::manager::CapabilitySlot>& slots);

  const hardware_runtime::manager::ResolvedCapability* Find(const std::string& name) const;
  const hardware_runtime::manager::ResolvedCapability* FindHardware(
      const std::string& primitive, const std::string& version,
      const std::string& device_id) const;
  const std::vector<hardware_runtime::manager::ResolvedCapability>& List() const {
    return capabilities_;
  }
  bool Available(const std::string& name) const;
  static std::string HardwareName(const std::string& robot_id, const std::string& device_class,
                                  const std::string& part_id, const std::string& primitive,
                                  const std::string& version);

 private:
  const hardware_runtime::Registry& registry_;
  std::vector<hardware_runtime::manager::ResolvedCapability> capabilities_;
};

}  // namespace eos::runtime::kernel

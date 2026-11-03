#pragma once

#include <string>
#include <vector>

#include "src/eos/runtime/hardware/registry.h"

namespace eos::runtime::hardware_runtime::manager {

// A stable, deployment-facing target. Generated device IDs never appear in
// package configuration; the runtime resolves them after hardware admission.
struct CapabilitySlot {
  std::string name;
  std::string robot_id;
  std::string device_class;
  std::string part_id;
  std::string primitive;
  std::string version;
};

struct ResolvedCapability {
  std::string name;
  std::string device_id;
  std::string primitive;
  std::string version;
};

// Fails closed on missing or ambiguous devices and on primitives absent from
// the admitted device's configuration. The result is suitable for minting
// execution-scoped worker grants; it grants nothing by itself.
std::vector<ResolvedCapability> ResolveCapabilitySlots(
    const Registry& registry, const std::vector<CapabilitySlot>& slots);

}  // namespace eos::runtime::hardware_runtime::manager

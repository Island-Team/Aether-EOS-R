#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/eos/runtime/hardware/mode.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/skills/core/provider.h"
#include "src/eos/runtime/skills/worker/grants.h"

namespace eos::runtime::init {

struct SkillCapability {
  std::string skill;
  std::string skill_version;
  std::string slot;
  std::string argument_pointer;
  std::string robot_id;
  std::string device_class;
  std::string primitive;
  std::string primitive_version;
};

struct SkillProviderDirectory {
  std::string module_id;
  std::filesystem::path directory;
};

struct RuntimeStack {
  std::vector<SkillCapability> skill_capabilities;
  std::vector<SkillProviderDirectory> skill_providers;
};

/// Loads the selected runtime graph and verifies that EOS can admit its hardware
/// binding. A package on disk grants no binding unless its module is listed by
/// the stack. Throws std::runtime_error for an inconsistent graph.
RuntimeStack ValidateRuntimeStack(const std::filesystem::path& stack_path,
                                  const std::filesystem::path& eos_config_path,
                                  const hardware_runtime::ModeConfig& mode);

/// Resolve an execution's stable part selectors against the admitted Registry.
/// Only declarations for the exact skill identity produce grants.
std::vector<skills::worker::Grant> ResolveSkillGrants(
    const std::vector<SkillCapability>& capabilities,
    const hardware_runtime::Registry& registry, const skills::Metadata& metadata,
    const nlohmann::json& arguments, const std::string& execution_id);

}  // namespace eos::runtime::init

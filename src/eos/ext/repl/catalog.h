/// Completion metadata assembled from local contracts and gateway discovery.
#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace eos::ext::repl {

struct ArgumentInfo {
  std::string description;
  std::vector<std::string> values;
  std::string type;
  bool required = false;
  bool has_default = false;
  nlohmann::json default_value;
  nlohmann::json schema;
};

class Catalog final {
 public:
  bool LoadContracts(const std::string& root, std::string* warning);
  void ClearRemote();
  void Observe(const std::string& method, const nlohmann::json& reply);

  std::vector<std::string> Resources() const;
  std::vector<std::string> Robots() const;
  std::vector<std::string> NavigationPointsFor(const std::string& robot) const;
  std::vector<std::string> DevicesForClass(const std::string& device_class) const;
  std::vector<std::string> Primitives() const;
  std::vector<std::string> PrimitiveVersionsFor(const std::string& primitive) const;
  std::vector<std::string> DevicesFor(const std::string& primitive,
                                      const std::string& version) const;
  std::map<std::string, ArgumentInfo> ArgumentsFor(const std::string& primitive,
                                                   const std::string& version,
                                                   const std::string& device = "") const;
  /// True when a remote binding explicitly supplied `parameters`, including
  /// an object schema with no properties, or a local fallback schema exists.
  bool HasPrimitiveParameterSchema(const std::string& primitive, const std::string& version,
                                   const std::string& device = "") const;
  std::string LimitHint(const std::string& primitive, const std::string& version,
                        const std::string& device, const std::string& argument) const;
  std::vector<std::string> Skills() const;
  std::vector<std::string> WorkflowTemplates() const;
  std::vector<std::string> SkillVersionsFor(const std::string& skill) const;
  std::map<std::string, ArgumentInfo> SkillArgumentsFor(const std::string& skill,
                                                         const std::string& version) const;
  std::vector<std::string> SkillArgumentValues(const std::string& argument,
                                               const ArgumentInfo& info) const;
  bool PrepareSkillLaunch(const std::string& skill, const std::string& requested_version,
                          nlohmann::json* arguments, std::string* resolved_version,
                          std::vector<std::string>* unresolved, std::string* error) const;
  std::vector<std::string> ExecutionIds() const;
  std::vector<std::string> ActiveExecutionIdsFor(const std::string& signal = "") const;
  std::vector<std::string> SignalNames() const;
  std::map<std::string, ArgumentInfo> SignalArgumentsFor(const std::string& execution_id,
                                                          const std::string& signal) const;
  bool Validate(const std::map<std::string, ArgumentInfo>& schema, const nlohmann::json& value,
                std::string* error) const;
  static bool ValidateWorkflow(const nlohmann::json& request, std::string* error);

  std::size_t ResourceCount() const { return resources_.size(); }
  std::size_t PrimitiveCount() const;

 private:
  std::set<std::string> resources_;
  std::set<std::string> robots_;
  std::map<std::string, std::set<std::string>> devices_by_class_;
  std::map<std::string, std::map<std::string, std::set<std::string>>> primitive_devices_;
  std::map<std::string, std::map<std::string, std::map<std::string, ArgumentInfo>>> arguments_;
  std::map<std::string, std::map<std::string, ArgumentInfo>> remote_arguments_;
  std::map<std::string, std::map<std::string, nlohmann::json>> limits_;
  std::map<std::string, std::vector<std::string>> references_;
  std::map<std::string, std::string> absolute_frames_;
  std::map<std::string, std::map<std::string, std::map<std::string, ArgumentInfo>>>
      skill_arguments_;
  std::map<std::string,
           std::map<std::string, std::map<std::string, std::map<std::string, ArgumentInfo>>>>
      signal_arguments_;
  struct ExecutionInfo {
    std::string skill;
    std::string version;
    std::string status;
  };
  std::map<std::string, ExecutionInfo> executions_;
  std::set<std::string> workflow_templates_;
  bool discovered_ = false;
};

}  // namespace eos::ext::repl

#include "catalog.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>

#include "src/eos/lib/log.h"

namespace eos::ext::repl {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

template <typename Collection>
std::vector<std::string> Copy(const Collection& values) {
  return {values.begin(), values.end()};
}

std::vector<std::string> SuggestedValues(const json& schema) {
  std::vector<std::string> values;
  if (schema.contains("enum") && schema.at("enum").is_array()) {
    for (const auto& value : schema.at("enum")) {
      values.push_back(value.is_string() ? value.get<std::string>() : value.dump());
    }
  } else if (schema.contains("const")) {
    const auto& value = schema.at("const");
    values.push_back(value.is_string() ? value.get<std::string>() : value.dump());
  } else if (schema.contains("type")) {
    const json& type = schema.at("type");
    if ((type.is_string() && type == "boolean") ||
        (type.is_array() && std::find(type.begin(), type.end(), "boolean") != type.end()))
      values = {"true", "false"};
  }
  return values;
}

std::string SchemaType(const json& schema) {
  if (schema.contains("type") && schema.at("type").is_string())
    return schema.at("type").get<std::string>();
  if (schema.contains("type") && schema.at("type").is_array()) {
    for (const auto& type : schema.at("type"))
      if (type.is_string() && type != "null") return type.get<std::string>();
  }
  return schema.contains("properties") ? "object" : "value";
}

std::map<std::string, ArgumentInfo> ArgumentsFromSchema(const json& schema) {
  std::map<std::string, ArgumentInfo> result;
  if (!schema.is_object() || !schema.contains("properties") || !schema.at("properties").is_object())
    return result;
  std::set<std::string> required;
  if (schema.contains("required") && schema.at("required").is_array())
    for (const auto& name : schema.at("required"))
      if (name.is_string()) required.insert(name.get<std::string>());
  for (const auto& [name, property] : schema.at("properties").items()) {
    if (!property.is_object()) continue;
    ArgumentInfo info;
    info.description = property.value("description", property.value("title", ""));
    info.values = SuggestedValues(property);
    info.type = SchemaType(property);
    info.required = required.count(name) != 0;
    info.has_default = property.contains("default");
    if (info.has_default) info.default_value = property.at("default");
    info.schema = property;
    result[name] = std::move(info);
  }
  return result;
}

bool TypeMatches(const std::string& type, const json& value) {
  if (type == "string") return value.is_string();
  if (type == "boolean") return value.is_boolean();
  if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
  if (type == "number") return value.is_number();
  if (type == "object") return value.is_object();
  if (type == "array") return value.is_array();
  return true;
}

}  // namespace

bool Catalog::LoadContracts(const std::string& root, std::string* warning) {
  arguments_.clear();
  if (root.empty()) return true;
  const fs::path directory = fs::path(root) / "ext/eos/actions";
  std::error_code ec;
  if (!fs::is_directory(directory, ec)) {
    if (warning)
      *warning = "contracts root has no ext/eos/actions directory: " + directory.string();
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                       "repl_catalog_load_failed", {{"reason", "contracts_directory_missing"}});
    return false;
  }
  try {
    for (const auto& entry : fs::directory_iterator(directory)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
      std::ifstream input(entry.path());
      json schema;
      input >> schema;
      if (!schema.contains("properties") || !schema.at("properties").is_object()) continue;
      const json& properties = schema.at("properties");
      if (!properties.contains("type") || !properties.at("type").is_object() ||
          !properties.at("type").contains("const") ||
          !properties.at("type").at("const").is_string()) {
        continue;
      }
      const std::string primitive = properties.at("type").at("const").get<std::string>();
      if (primitive == "primitive.ack") continue;
      const std::string version = schema.value("x-island-primitive-version", "");
      if (version.empty()) continue;
      auto arguments = ArgumentsFromSchema(schema);
      arguments.erase("type");
      arguments.erase("command_id");
      arguments_[primitive][version] = std::move(arguments);
    }
  } catch (const std::exception& error) {
    arguments_.clear();
    if (warning)
      *warning = "cannot load primitive completion schemas: " + std::string(error.what());
    eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::warn,
                       "repl_catalog_load_failed", {{"reason", "schema_parse_failed"}});
    return false;
  }
  eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::debug, "repl_catalog_loaded",
                     {{"primitive_count", arguments_.size()}});
  return true;
}

void Catalog::ClearRemote() {
  const std::size_t resource_count = resources_.size();
  const std::size_t primitive_count = primitive_devices_.size();
  resources_.clear();
  robots_.clear();
  devices_by_class_.clear();
  primitive_devices_.clear();
  remote_arguments_.clear();
  limits_.clear();
  references_.clear();
  absolute_frames_.clear();
  skill_arguments_.clear();
  signal_arguments_.clear();
  executions_.clear();
  workflow_templates_.clear();
  discovered_ = false;
  eos::lib::LogEvent(eos::lib::GetLogger("eos-repl"), spdlog::level::debug, "repl_catalog_cleared",
                     {{"resource_count", resource_count}, {"primitive_count", primitive_count}});
}

void Catalog::Observe(const std::string& method, const json& reply) {
  if (reply.is_object() && reply.contains("error")) return;
  if (method == "Discover" && reply.is_object() &&
      (reply.contains("robot") || reply.contains("devices"))) {
    discovered_ = true;
    robots_.clear();
    devices_by_class_.clear();
    primitive_devices_.clear();
    remote_arguments_.clear();
    limits_.clear();
    references_.clear();
    absolute_frames_.clear();
    const auto ingest_bindings = [&](const json& bindings, const std::string& target) {
      if (!bindings.is_array() || target.empty()) return;
      for (const auto& binding : bindings) {
        if (!binding.is_object()) continue;
        const std::string name = binding.value("name", "");
        const std::string version = binding.value("version", "");
        if (name.empty() || version.empty()) continue;
        primitive_devices_[name][version].insert(target);
        const std::string key = name + "\n" + version + "\n" + target;
        if (binding.contains("parameters") && binding.at("parameters").is_object())
          remote_arguments_[key] = ArgumentsFromSchema(binding.at("parameters"));
        if (binding.contains("limits") && binding.at("limits").is_object()) {
          for (const auto& [argument, limit] : binding.at("limits").items()) {
            limits_[key][argument] = limit;
          }
        }
        if (binding.contains("references") && binding.at("references").is_array()) {
          for (const auto& reference : binding.at("references")) {
            if (reference.is_string()) references_[key].push_back(reference.get<std::string>());
          }
        }
        const std::string absolute_frame = binding.value("absoluteFrame", "");
        if (!absolute_frame.empty()) absolute_frames_[key] = absolute_frame;
      }
    };
    if (reply.contains("robot") && reply.at("robot").is_object()) {
      const json& robot = reply.at("robot");
      const std::string name = robot.value("name", "");
      if (!name.empty()) robots_.insert(name);
      if (robot.contains("primitives")) ingest_bindings(robot.at("primitives"), "-");
    }
    const json devices = reply.value("devices", json::array());
    if (!devices.is_array()) return;
    for (const auto& device : devices) {
      if (!device.is_object()) continue;
      const std::string device_id = device.value("id", "");
      const std::string device_class = device.value("class", "");
      if (!device_id.empty() && !device_class.empty()) {
        devices_by_class_[device_class].insert(device_id);
      }
      if (device.contains("primitives")) ingest_bindings(device.at("primitives"), device_id);
    }
  } else if (method == "ListWorkflowTemplates" && reply.is_object()) {
    workflow_templates_.clear();
    for (const auto& descriptor : reply.value("templates", json::array()))
      if (descriptor.is_object() && descriptor.contains("name") &&
          descriptor.at("name").is_string())
        workflow_templates_.insert(descriptor.at("name").get<std::string>());
  } else if ((method == "ListSkills" || method == "GetSkill") && reply.is_object()) {
    const auto ingest = [&](const json& descriptor) {
      if (!descriptor.is_object()) return;
      const json reference = descriptor.value("reference", json::object());
      const std::string name = reference.value("name", "");
      const std::string version = reference.value("version", "");
      if (name.empty() || version.empty()) return;
      skill_arguments_[name][version] =
          ArgumentsFromSchema(descriptor.value("parameters", json::object()));
      const json signals = descriptor.value("acceptedSignals", json::array());
      if (signals.is_array()) {
        for (const auto& signal : signals) {
          const std::string signal_name = signal.value("name", "");
          if (!signal_name.empty())
            signal_arguments_[name][version][signal_name] =
                ArgumentsFromSchema(signal.value("payloadSchema", json::object()));
        }
      }
    };
    if (method == "GetSkill") ingest(reply);
    const json skills = reply.value("skills", json::array());
    if (skills.is_array())
      for (const auto& skill : skills) ingest(skill);
  } else if ((method == "StartExecution" || method == "GetExecution") && reply.is_object()) {
    const json reference = reply.value("reference", json::object());
    const std::string id = reply.value("executionId", "");
    if (!id.empty())
      executions_[id] = {reference.value("name", ""), reference.value("version", ""),
                         reply.value("status", "")};
  } else if ((method == "ListExecutions" || method == "PsExecutions") && reply.is_object()) {
    const json executions = reply.value("executions", json::array());
    if (executions.is_array())
      for (const auto& execution : executions) {
        const json reference = execution.value("reference", json::object());
        const std::string id = execution.value("executionId", "");
        if (!id.empty())
          executions_[id] = {reference.value("name", ""), reference.value("version", ""),
                             execution.value("status", "")};
      }
  } else if (method == "ListResources" && reply.is_array()) {
    resources_.clear();
    for (const auto& resource : reply) {
      if (resource.is_object() && resource.contains("uri") && resource.at("uri").is_string()) {
        resources_.insert(resource.at("uri").get<std::string>());
      }
    }
  }
}

std::vector<std::string> Catalog::WorkflowTemplates() const {
  return Copy(workflow_templates_);
}

std::vector<std::string> Catalog::Skills() const {
  std::set<std::string> result;
  for (const auto& [name, unused] : skill_arguments_) result.insert(name);
  return Copy(result);
}

std::vector<std::string> Catalog::SkillVersionsFor(const std::string& skill) const {
  const auto found = skill_arguments_.find(skill);
  if (found == skill_arguments_.end()) return {};
  std::set<std::string> result;
  for (const auto& [version, unused] : found->second) result.insert(version);
  return Copy(result);
}

std::vector<std::string> Catalog::ExecutionIds() const {
  std::vector<std::string> ids;
  ids.reserve(executions_.size());
  for (const auto& [id, unused] : executions_) ids.push_back(id);
  return ids;
}

std::vector<std::string> Catalog::SignalNames() const {
  std::set<std::string> names{"cancel"};
  for (const auto& [skill, versions] : signal_arguments_)
    for (const auto& [version, signals] : versions)
      for (const auto& [name, arguments] : signals) {
        if (name != "skill.command") {
          names.insert(name);
          continue;
        }
        const auto command = arguments.find("command");
        if (command == arguments.end()) continue;
        for (const auto& value : command->second.values)
          if (value == "pause" || value == "resume") names.insert(value);
      }
  return Copy(names);
}

std::vector<std::string> Catalog::ActiveExecutionIdsFor(const std::string& signal) const {
  static const std::set<std::string> terminal = {
      "SUCCEEDED", "REJECTED", "CANCELLED", "TIMED_OUT", "FAILED"};
  std::vector<std::string> ids;
  for (const auto& [id, execution] : executions_) {
    const std::string status = execution.status.rfind("EXECUTION_STATUS_", 0) == 0
                                   ? execution.status.substr(17)
                                   : execution.status;
    if (terminal.count(status) != 0) continue;
    if (signal.empty() || signal == "cancel") {
      ids.push_back(id);
      continue;
    }
    const std::string declared = signal == "pause" || signal == "resume" ? "skill.command" : signal;
    const auto skill = signal_arguments_.find(execution.skill);
    if (skill == signal_arguments_.end()) continue;
    const auto version = skill->second.find(execution.version);
    if (version == skill->second.end()) continue;
    const auto found = version->second.find(declared);
    if (found == version->second.end()) continue;
    if (signal == "pause" || signal == "resume") {
      const auto command = found->second.find("command");
      if (command == found->second.end() ||
          std::find(command->second.values.begin(), command->second.values.end(), signal) ==
              command->second.values.end())
        continue;
    }
    ids.push_back(id);
  }
  return ids;
}

std::map<std::string, ArgumentInfo> Catalog::SkillArgumentsFor(const std::string& skill,
                                                               const std::string& version) const {
  const auto found = skill_arguments_.find(skill);
  if (found == skill_arguments_.end()) return {};
  const auto version_found = found->second.find(version);
  return version_found == found->second.end() ? std::map<std::string, ArgumentInfo>{}
                                               : version_found->second;
}

std::vector<std::string> Catalog::SkillArgumentValues(const std::string& argument,
                                                      const ArgumentInfo& info) const {
  if (!info.values.empty()) return info.values;
  if (argument == "robot_name") return Robots();
  constexpr char kDeviceSuffix[] = "_device_id";
  if (argument.size() > sizeof(kDeviceSuffix) - 1 &&
      argument.compare(argument.size() - (sizeof(kDeviceSuffix) - 1), sizeof(kDeviceSuffix) - 1,
                       kDeviceSuffix) == 0) {
    return DevicesForClass(argument.substr(0, argument.size() - (sizeof(kDeviceSuffix) - 1)));
  }
  return {};
}

bool Catalog::PrepareSkillLaunch(const std::string& skill, const std::string& requested_version,
                                 json* arguments, std::string* resolved_version,
                                 std::vector<std::string>* unresolved, std::string* error) const {
  const std::vector<std::string> versions = SkillVersionsFor(skill);
  if (versions.empty()) {
    *error = "unknown skill '" + skill + "'";
    return false;
  }
  if (requested_version.empty()) {
    if (versions.size() != 1) {
      *error = "skill '" + skill + "' has multiple versions; specify one";
      return false;
    }
    *resolved_version = versions.front();
  } else {
    if (std::find(versions.begin(), versions.end(), requested_version) == versions.end()) {
      *error = "unknown version '" + requested_version + "' for skill '" + skill + "'";
      return false;
    }
    *resolved_version = requested_version;
  }

  unresolved->clear();
  for (const auto& [name, info] : SkillArgumentsFor(skill, *resolved_version)) {
    if (!info.required || arguments->contains(name)) continue;
    if (info.has_default) {
      (*arguments)[name] = info.default_value;
      continue;
    }
    const std::vector<std::string> values = SkillArgumentValues(name, info);
    if (values.size() == 1)
      (*arguments)[name] = values.front();
    else
      unresolved->push_back(name);
  }
  return true;
}

std::map<std::string, ArgumentInfo> Catalog::SignalArgumentsFor(const std::string& execution_id,
                                                                 const std::string& signal) const {
  const auto execution = executions_.find(execution_id);
  if (execution == executions_.end()) return {};
  const auto skill = signal_arguments_.find(execution->second.skill);
  if (skill == signal_arguments_.end()) return {};
  const auto version = skill->second.find(execution->second.version);
  if (version == skill->second.end()) return {};
  const auto found = version->second.find(signal);
  return found == version->second.end() ? std::map<std::string, ArgumentInfo>{} : found->second;
}

bool Catalog::Validate(const std::map<std::string, ArgumentInfo>& schema, const json& value,
                       std::string* error) const {
  if (!value.is_object()) {
    *error = "value must be an object";
    return false;
  }
  for (const auto& [name, info] : schema) {
    if (info.required && !value.contains(name)) {
      *error = "required field '" + name + "' is missing";
      return false;
    }
  }
  for (const auto& [name, field] : value.items()) {
    const auto found = schema.find(name);
    if (found == schema.end()) {
      *error = "unknown field '" + name + "'";
      return false;
    }
    const ArgumentInfo& info = found->second;
    if (!TypeMatches(info.type, field)) {
      *error = "field '" + name + "' must be " + info.type;
      return false;
    }
    if (!info.values.empty() && std::find(info.values.begin(), info.values.end(),
                                          field.is_string() ? field.get<std::string>()
                                                            : field.dump()) == info.values.end()) {
      *error = "field '" + name + "' is not one of the allowed choices";
      return false;
    }
    if (field.is_number() && info.schema.is_object()) {
      const double number = field.get<double>();
      if (info.schema.contains("minimum") && number < info.schema.at("minimum").get<double>()) {
        *error = "field '" + name + "' is below its minimum";
        return false;
      }
      if (info.schema.contains("maximum") && number > info.schema.at("maximum").get<double>()) {
        *error = "field '" + name + "' exceeds its maximum";
        return false;
      }
    }
  }
  return true;
}

bool Catalog::ValidateWorkflow(const json& request, std::string* error) {
  if (!request.is_object() || !request.contains("definition") ||
      !request.at("definition").is_object()) {
    *error = "workflow requires an object definition";
    return false;
  }
  const json& definition = request.at("definition");
  if (!definition.contains("workflowId") || !definition.at("workflowId").is_string() ||
      definition.at("workflowId").get<std::string>().empty()) {
    *error = "definition.workflowId must be a non-empty string";
    return false;
  }
  if (!definition.contains("actions") || !definition.at("actions").is_array() ||
      definition.at("actions").empty()) {
    *error = "definition.actions must be a non-empty array";
    return false;
  }
  if (definition.contains("supersedesWorkflowIds")) {
    const auto& supersedes = definition.at("supersedesWorkflowIds");
    std::set<std::string> unique;
    if (!supersedes.is_array()) {
      *error = "definition.supersedesWorkflowIds must be an array";
      return false;
    }
    for (const auto& id : supersedes) {
      if (!id.is_string() || id.get<std::string>().empty() || id == definition.at("workflowId") ||
          !unique.insert(id.get<std::string>()).second) {
        *error = "superseded workflow IDs must be unique, non-empty, and not self-referential";
        return false;
      }
    }
  }

  std::set<std::string> ids;
  std::map<std::string, std::vector<std::string>> dependencies;
  for (const auto& action : definition.at("actions")) {
    if (!action.is_object() || !action.contains("actionId") || !action.at("actionId").is_string() ||
        action.at("actionId").get<std::string>().empty() ||
        !ids.insert(action.at("actionId").get<std::string>()).second) {
      *error = "action IDs must be unique non-empty strings";
      return false;
    }
    const bool skill = action.contains("skill");
    const bool primitive = action.contains("primitive");
    const bool control = action.contains("control");
    const bool noop = action.contains("noop");
    if (static_cast<int>(skill) + static_cast<int>(primitive) + static_cast<int>(control) +
            static_cast<int>(noop) !=
        1) {
      *error = "each action requires exactly one target";
      return false;
    }
    if (action.contains("parameters") && !action.at("parameters").is_object()) {
      *error = "action parameters must be an object";
      return false;
    }
    if (skill || primitive) {
      const auto& target = action.at(skill ? "skill" : "primitive");
      if (!target.is_object() || !target.contains("name") || !target.at("name").is_string() ||
          target.at("name").get<std::string>().empty() || !target.contains("version") ||
          !target.at("version").is_string() || target.at("version").get<std::string>().empty() ||
          (primitive && (!target.contains("deviceId") || !target.at("deviceId").is_string()))) {
        *error = "each action target requires name and version; primitives also require deviceId";
        return false;
      }
    } else if (control) {
      const auto& target = action.at("control");
      if (!target.is_object() || !target.contains("targetActionId") ||
          !target.at("targetActionId").is_string() ||
          target.at("targetActionId").get<std::string>().empty() ||
          !target.contains("operation")) {
        *error = "control target requires targetActionId and operation";
        return false;
      }
    } else if (!action.at("noop").is_object()) {
      *error = "noop target must be an object";
      return false;
    }
    if (primitive && action.contains("parameters") &&
        action.at("parameters").contains("commandId")) {
      *error = "primitive workflow parameters must not contain commandId";
      return false;
    }
    auto& action_dependencies = dependencies[action.at("actionId").get<std::string>()];
    if (action.contains("dependsOn")) {
      std::set<std::string> unique;
      if (!action.at("dependsOn").is_array()) {
        *error = "action dependsOn must be an array";
        return false;
      }
      for (const auto& dependency : action.at("dependsOn")) {
        if (!dependency.is_string() || dependency.get<std::string>().empty() ||
            !unique.insert(dependency.get<std::string>()).second) {
          *error = "action dependencies must be unique non-empty strings";
          return false;
        }
        action_dependencies.push_back(dependency.get<std::string>());
      }
    }
  }
  for (const auto& [id, action_dependencies] : dependencies) {
    for (const auto& dependency : action_dependencies) {
      if (dependency == id || ids.count(dependency) == 0) {
        *error = "action dependency must name another action in the workflow";
        return false;
      }
    }
  }
  std::map<std::string, int> visit_state;
  std::function<bool(const std::string&)> has_cycle = [&](const std::string& id) {
    if (visit_state[id] == 1) return true;
    if (visit_state[id] == 2) return false;
    visit_state[id] = 1;
    for (const auto& dependency : dependencies[id])
      if (has_cycle(dependency)) return true;
    visit_state[id] = 2;
    return false;
  };
  for (const auto& id : ids) {
    if (has_cycle(id)) {
      *error = "workflow action dependencies must be acyclic";
      return false;
    }
  }
  return true;
}

std::vector<std::string> Catalog::Resources() const { return Copy(resources_); }

std::vector<std::string> Catalog::NavigationPointsFor(const std::string& robot) const {
  const std::string prefix = "robots/" + robot + "/navigation/waypoints/";
  std::vector<std::string> points;
  for (const std::string& resource : resources_) {
    if (resource.rfind(prefix, 0) == 0 && resource.size() > prefix.size())
      points.push_back(resource.substr(prefix.size()));
  }
  return points;
}

std::vector<std::string> Catalog::Robots() const { return Copy(robots_); }

std::vector<std::string> Catalog::DevicesForClass(const std::string& device_class) const {
  const auto found = devices_by_class_.find(device_class);
  return found == devices_by_class_.end() ? std::vector<std::string>{} : Copy(found->second);
}

std::size_t Catalog::PrimitiveCount() const {
  std::size_t count = 0;
  for (const auto& [unused, versions] : primitive_devices_) count += versions.size();
  return count;
}

std::vector<std::string> Catalog::Primitives() const {
  std::set<std::string> values;
  for (const auto& [name, unused] : primitive_devices_) values.insert(name);
  if (!discovered_)
    for (const auto& [name, unused] : arguments_) values.insert(name);
  return Copy(values);
}

std::vector<std::string> Catalog::PrimitiveVersionsFor(const std::string& primitive) const {
  std::set<std::string> versions;
  if (const auto found = primitive_devices_.find(primitive); found != primitive_devices_.end())
    for (const auto& [version, unused] : found->second) versions.insert(version);
  if (!discovered_)
    if (const auto found = arguments_.find(primitive); found != arguments_.end())
      for (const auto& [version, unused] : found->second) versions.insert(version);
  return Copy(versions);
}

std::vector<std::string> Catalog::DevicesFor(const std::string& primitive,
                                             const std::string& version) const {
  const auto found = primitive_devices_.find(primitive);
  if (found == primitive_devices_.end()) return {};
  const auto version_found = found->second.find(version);
  return version_found == found->second.end() ? std::vector<std::string>{}
                                              : Copy(version_found->second);
}

std::map<std::string, ArgumentInfo> Catalog::ArgumentsFor(const std::string& primitive,
                                                          const std::string& version,
                                                          const std::string& device) const {
  const std::string key = primitive + "\n" + version + "\n" + device;
  if (const auto remote = remote_arguments_.find(key); remote != remote_arguments_.end())
    return remote->second;
  if (device.empty()) {
    const auto fleet = remote_arguments_.find(primitive + "\n" + version + "\n-");
    if (fleet != remote_arguments_.end()) return fleet->second;
  }
  const auto found = arguments_.find(primitive);
  if (found == arguments_.end()) return {};
  const auto version_found = found->second.find(version);
  if (version_found == found->second.end()) return {};
  auto arguments = version_found->second;
  if (const auto reference = arguments.find("reference"); reference != arguments.end()) {
    const auto admitted = references_.find(key);
    if (admitted != references_.end()) reference->second.values = admitted->second;
  }
  if (const auto frame = arguments.find("frame_id"); frame != arguments.end()) {
    const auto admitted = absolute_frames_.find(key);
    if (admitted != absolute_frames_.end()) frame->second.values = {admitted->second};
  }
  return arguments;
}

bool Catalog::HasPrimitiveParameterSchema(const std::string& primitive,
                                          const std::string& version,
                                          const std::string& device) const {
  const std::string key = primitive + "\n" + version + "\n" + device;
  if (remote_arguments_.count(key) != 0) return true;
  if (device.empty() && remote_arguments_.count(primitive + "\n" + version + "\n-") != 0)
    return true;
  const auto found = arguments_.find(primitive);
  return found != arguments_.end() && found->second.count(version) != 0;
}

std::string Catalog::LimitHint(const std::string& primitive, const std::string& version,
                               const std::string& device, const std::string& argument) const {
  const auto descriptor = limits_.find(primitive + "\n" + version + "\n" + device);
  if (descriptor == limits_.end()) return {};
  const auto found = descriptor->second.find(argument);
  if (found == descriptor->second.end() || !found->second.is_object()) return {};
  const json& limit = found->second;
  const bool has_minimum = limit.contains("minimum") && limit.at("minimum").is_number();
  const bool has_maximum = limit.contains("maximum") && limit.at("maximum").is_number();
  if (!has_minimum && !has_maximum) return {};
  const std::string minimum = has_minimum ? limit.at("minimum").dump() : "-inf";
  const std::string maximum = has_maximum ? limit.at("maximum").dump() : "+inf";
  const char left =
      limit.value("minimumExclusive", limit.value("minimum_exclusive", false)) ? '(' : '[';
  const char right =
      limit.value("maximumExclusive", limit.value("maximum_exclusive", false)) ? ')' : ']';
  return std::string("range ") + left + minimum + ", " + maximum + right;
}

}  // namespace eos::ext::repl

#include "src/eos/runtime/northbound/workflow/registry.h"

#include <google/protobuf/util/json_util.h>

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::northbound {
namespace {

namespace fs = std::filesystem;
namespace Proto = island::eos::v1;

bool DetectionValuePointerAllowed(const std::string& pointer) {
  static const std::set<std::string> kAllowed = {
      "/id",                         "/label",
      "/confidence",                 "/bbox",
      "/distance_m",                 "/bearing_rad",
      "/attributes/name",            "/attributes/pose",
      "/attributes/pose_confidence", "/attributes/posture_state",
      "/attributes/action",          "/attributes/action_confidence",
      "/attributes/activity",        "/attributes/speaking"};
  return kAllowed.count(pointer) != 0;
}

void ValidateTemplate(const Proto::WorkflowTemplateDescriptor& descriptor,
                      const fs::path& path) {
  const auto& definition = descriptor.definition();
  if (definition.workflow_id().empty() || definition.actions().empty())
    throw std::runtime_error("workflow template '" + path.string() +
                             "' requires workflowId and actions");
  std::set<std::string> action_ids;
  for (const auto& action : definition.actions()) {
    if (action.action_id().empty() || !action.has_parameters() ||
        action.target_case() == Proto::WorkflowAction::TARGET_NOT_SET)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has an action without ID, target, or parameters");
    if (!action_ids.insert(action.action_id()).second)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has duplicate action IDs");
    const bool target_missing =
        (action.has_skill() &&
         (action.skill().name().empty() || action.skill().version().empty())) ||
        (action.has_primitive() &&
         (action.primitive().name().empty() || action.primitive().version().empty())) ||
        (action.has_control() &&
         (action.control().target_action_id().empty() ||
          action.control().operation() != Proto::ACTION_CONTROL_OPERATION_TERMINATE));
    if (target_missing)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has an action target without name or version");
    if (action.has_when()) {
      if (!action.when().resource_uri().empty()) {
        if (!action.when().fact_key().empty() || !action.when().source_action_id().empty() ||
            action.when().detection_kind().empty() ||
            action.when().detection_value_pointer().empty() || !action.when().has_equals())
          throw std::runtime_error("workflow template '" + path.string() +
                                   "' has a resource condition with conflicting fields");
        if (action.when().detection_kind() != "human" &&
            action.when().detection_kind() != "object")
          throw std::runtime_error("workflow template '" + path.string() +
                                   "' has an invalid resource condition detection_kind");
        if (!DetectionValuePointerAllowed(action.when().detection_value_pointer()))
          throw std::runtime_error(
              "workflow template '" + path.string() +
              "' has an invalid resource condition detection_value_pointer");
      } else if (action.when().fact_key().empty() || !action.when().has_equals()) {
        throw std::runtime_error("workflow template '" + path.string() +
                                 "' has an invalid state condition");
      }
    }
    if (!action.choice_group().empty() && !action.has_when())
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has a choice action without a state condition");
    if (action.monitor_lifetime() != Proto::MONITOR_LIFETIME_UNSPECIFIED &&
        action.execution_mode() != Proto::ACTION_EXECUTION_MODE_MONITOR)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' assigns monitor lifetime to a non-monitor action");
    if (action.execution_mode() == Proto::ACTION_EXECUTION_MODE_MONITOR &&
        action.monitor_lifetime() == Proto::MONITOR_LIFETIME_UNSPECIFIED)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has a monitor without a lifetime");
    if ((action.has_control() || action.has_noop()) &&
        action.execution_mode() == Proto::ACTION_EXECUTION_MODE_MONITOR)
      throw std::runtime_error("workflow template '" + path.string() +
                               "' uses a control or noop action as a monitor");
  }

  std::map<std::string, std::size_t> indegree;
  std::map<std::string, std::vector<std::string>> dependents;
  for (const auto& id : action_ids) indegree[id] = 0;
  for (const auto& action : definition.actions()) {
    if (action.has_when() && !action.when().source_action_id().empty() &&
        (!action_ids.count(action.when().source_action_id()) ||
         action.when().source_action_id() == action.action_id()))
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has an unknown or self condition source action");
    if (action.has_control() &&
        (!action_ids.count(action.control().target_action_id()) ||
         action.control().target_action_id() == action.action_id()))
      throw std::runtime_error("workflow template '" + path.string() +
                               "' has an unknown or self control target action");
    std::set<std::string> unique;
    for (const auto& dependency : action.depends_on()) {
      if (!action_ids.count(dependency) || dependency == action.action_id() ||
          !unique.insert(dependency).second)
        throw std::runtime_error("workflow template '" + path.string() +
                                 "' has an unknown, self, or duplicate dependency");
      ++indegree[action.action_id()];
      dependents[dependency].push_back(action.action_id());
    }
  }
  std::vector<std::string> ready;
  for (const auto& [id, degree] : indegree)
    if (degree == 0) ready.push_back(id);
  std::size_t visited = 0;
  while (!ready.empty()) {
    const std::string id = ready.back();
    ready.pop_back();
    ++visited;
    for (const auto& dependent : dependents[id])
      if (--indegree[dependent] == 0) ready.push_back(dependent);
  }
  if (visited != action_ids.size())
    throw std::runtime_error("workflow template '" + path.string() +
                             "' contains a dependency cycle");
}

}  // namespace

WorkflowTemplateRegistry::WorkflowTemplateRegistry(const fs::path& directory) {
  if (directory.empty()) return;
  std::error_code ec;
  if (!fs::is_directory(directory, ec))
    throw std::runtime_error("workflow template directory is unavailable: " +
                             directory.string());
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    const std::string name = entry.path().stem().string();
    std::ifstream input(entry.path(), std::ios::binary);
    if (!input) throw std::runtime_error("cannot read workflow template: " + entry.path().string());
    std::ostringstream contents;
    contents << input.rdbuf();
    Proto::WorkflowTemplateDescriptor descriptor;
    descriptor.set_name(name);
    const auto status = google::protobuf::util::JsonStringToMessage(
        contents.str(), descriptor.mutable_definition());
    if (!status.ok())
      throw std::runtime_error("invalid workflow template '" + entry.path().string() + "': " +
                               status.ToString());
    ValidateTemplate(descriptor, entry.path());
    if (!templates_.emplace(name, std::move(descriptor)).second)
      throw std::runtime_error("duplicate workflow template name: " + name);
  }
  eos::lib::LogEvent(eos::lib::GetLogger("workflow"), spdlog::level::info,
                     "workflow_templates_registered",
                     {{"directory", directory.string()}, {"count", templates_.size()}});
}

std::vector<Proto::WorkflowTemplateDescriptor> WorkflowTemplateRegistry::List() const {
  std::vector<Proto::WorkflowTemplateDescriptor> result;
  result.reserve(templates_.size());
  for (const auto& [name, descriptor] : templates_) result.push_back(descriptor);
  return result;
}

std::optional<Proto::WorkflowTemplateDescriptor> WorkflowTemplateRegistry::Get(
    const std::string& name) const {
  const auto found = templates_.find(name);
  if (found == templates_.end()) return std::nullopt;
  return found->second;
}

}  // namespace eos::runtime::northbound

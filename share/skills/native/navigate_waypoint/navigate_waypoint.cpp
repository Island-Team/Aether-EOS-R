#include "share/skills/native/navigate_waypoint/navigate_waypoint.h"

#include <chrono>
#include <cmath>
#include <string>

#include "share/skills/native/common/primitive_support.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::navigate_waypoint {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
using namespace std::chrono_literals;
namespace {

bool ValidTarget(const nlohmann::json& target) {
  if (!target.is_object()) return false;
  const std::string reference = target.value("reference", "");
  if (reference != "relative" && reference != "absolute") return false;
  if (reference == "absolute" &&
      (!target.contains("frame_id") || !target.at("frame_id").is_string() ||
       target.at("frame_id").get_ref<const std::string&>().empty()))
    return false;
  if (reference == "relative" && target.contains("frame_id")) return false;
  for (const char* key : {"x_m", "y_m", "yaw_rad"}) {
    if (!target.contains(key) || !target.at(key).is_number() ||
        !std::isfinite(target.at(key).get<double>()))
      return false;
  }
  return true;
}

Task<Outcome> Navigate(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot_name = arguments.at("robot_name").get<std::string>();
  const std::string base_device_id = arguments.at("base_device_id").get<std::string>();
  const std::string waypoint_id = arguments.at("waypoint_id").get<std::string>();

  nlohmann::json target;
  if (arguments.contains("target")) {
    target = arguments.at("target");
  } else {
    auto configured = execution.ReadResource(
        "robots/" + robot_name + "/navigation/waypoints/" + waypoint_id);
    if (!configured)
      co_return Outcome::Failed("waypoint.unavailable", configured.error().message);
    target = std::move(configured.value());
  }
  if (!ValidTarget(target))
    co_return Outcome::InvalidArgument("waypoint.invalid", "waypoint target is invalid");
  if (execution.cancelled()) co_return Outcome::Cancelled();

  (void)execution.Checkpoint({"navigation.started", {}, 0.1, {}});
  auto task = island::skills::primitive::Run(
      execution, {"base.navigate", "1.0.0"}, base_device_id, target);
  const auto result = co_await task;
  using PrimitiveStatus = island::skills::primitive::Status;
  if (result.status != PrimitiveStatus::kSucceeded) {
    if (result.status == PrimitiveStatus::kCancelled)
      co_return Outcome::Cancelled("cancelled", result.message);
    if (result.status == PrimitiveStatus::kRejected) {
      Outcome rejected =
          Outcome::Rejected("navigation.rejected", result.message);
      rejected.result = {{"waypoint_id", waypoint_id}, {"target", target}};
      co_return rejected;
    }
    const char* suffix = result.status == PrimitiveStatus::kTimedOut         ? "timed_out"
                         : result.status == PrimitiveStatus::kOutcomeUnknown ? "outcome_unknown"
                                                                             : "failed";
    co_return Outcome::Failed(std::string("navigation.") + suffix, result.message);
  }

  (void)execution.Checkpoint({"navigation.completed", {}, 1.0, {}});
  co_return Outcome::Completed(
      {{"waypoint_id", waypoint_id}, {"target", std::move(target)}});
}

}  // namespace

void RegisterSkill(Registry& registry) {
  Definition definition;
  definition.metadata.name = "navigate_waypoint";
  definition.metadata.version = "1.0.0";
  definition.metadata.description =
      "Resolves a named waypoint and executes it through base.navigate.";
  definition.metadata.library = "island.navigate_waypoint";
  definition.metadata.library_version = "1.0.0";
  definition.metadata.build_id = "island-source";
  definition.metadata.parameters_schema = {
      {"type", "object"},
      {"properties",
       {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
        {"base_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"waypoint_id", {{"type", "string"}, {"minLength", 1}}},
        {"target",
         {{"type", "object"},
          {"properties",
           {{"reference", {{"type", "string"}, {"enum", {"relative", "absolute"}}}},
            {"frame_id", {{"type", "string"}, {"minLength", 1}}},
            {"x_m", {{"type", "number"}}},
            {"y_m", {{"type", "number"}}},
            {"yaw_rad", {{"type", "number"}}}}},
          {"required", {"reference", "x_m", "y_m", "yaw_rad"}},
          {"additionalProperties", false}}}}},
      {"required", {"robot_name", "base_device_id", "waypoint_id"}},
      {"additionalProperties", false}};
  definition.metadata.returns_schema = {
      {"type", "object"},
      {"properties",
       {{"waypoint_id", {{"type", "string"}, {"minLength", 1}}},
        {"target", {{"type", "object"}}}}},
      {"required", {"waypoint_id", "target"}},
      {"additionalProperties", false}};
  definition.metadata.primitives.push_back({"base.navigate", "1.0.0"});
  definition.metadata.claim_templates.push_back(
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/base_device_id"});
  definition.metadata.resources.push_back(
      {"robots/{robot_name}/navigation/waypoints/{waypoint_id}",
       "Configured absolute or relative navigation target."});
  definition.metadata.reentrant = true;
  definition.handler = Navigate;
  registry.Register(std::move(definition));
}

}  // namespace island::skills::native::navigate_waypoint

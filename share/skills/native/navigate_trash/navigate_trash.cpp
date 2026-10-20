#include "share/skills/native/navigate_trash/navigate_trash.h"

#include <chrono>
#include <cmath>
#include <optional>
#include <string>

#include "share/skills/native/common/navigation_trash_support.h"
#include "share/skills/native/common/primitive_support.h"
#include "share/skills/native/pick_trash/pick_trash.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::navigate_trash {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {

bool ValidAbsoluteTarget(const nlohmann::json& target) {
  if (!target.is_object() || target.value("reference", "") != "absolute" ||
      target.value("frame_id", "") != "map" || target.size() != 5)
    return false;
  for (const char* key : {"x_m", "y_m", "yaw_rad"}) {
    if (!target.contains(key) || !target.at(key).is_number() ||
        !std::isfinite(target.at(key).get<double>()))
      return false;
  }
  return true;
}

Result<nlohmann::json> ResolveTarget(Execution& execution, const nlohmann::json& arguments) {
  const bool waypoint = arguments.contains("start_waypoint_id");
  const bool relative = arguments.contains("start_relative_target");
  if (waypoint == relative)
    return Result<nlohmann::json>::Failure(
        {"navigation.target_invalid", "exactly one navigation target is required"});

  const std::string robot = arguments.at("robot_name").get<std::string>();
  if (waypoint) {
    const std::string waypoint_id = arguments.at("start_waypoint_id").get<std::string>();
    auto target = execution.ReadResource("robots/" + robot + "/navigation/waypoints/" +
                                         waypoint_id);
    if (!target) return Result<nlohmann::json>::Failure(target.error());
    if (!ValidAbsoluteTarget(target.value()))
      return Result<nlohmann::json>::Failure(
          {"waypoint.invalid", "waypoint must be an absolute map target"});
    return Result<nlohmann::json>::Success(std::move(target.value()));
  }

  auto status = execution.ReadResource("robots/" + robot + "/base/status");
  if (!status) return Result<nlohmann::json>::Failure(status.error());
  auto target = navigation_trash::AbsoluteMapTarget(
      status.value(), arguments.at("start_relative_target"));
  if (!target)
    return Result<nlohmann::json>::Failure(
        {"navigation.localization_unavailable", "fresh localized map pose is required"});
  return Result<nlohmann::json>::Success(std::move(*target));
}

Outcome PrimitiveFailure(const char* phase, const primitive::Result& result) {
  if (result.status == primitive::Status::kCancelled) return Outcome::Cancelled();
  return Outcome::Failed(std::string(phase) + ".failed", result.message);
}

Task<primitive::Result> WaitForPrimitive(Execution& execution,
                                         const PrimitiveOperation& operation) {
  using namespace std::chrono_literals;
  while (true) {
    auto waited = execution.WaitPrimitive(operation, 50ms);
    if (!waited)
      co_return primitive::Result{primitive::Status::kFailed, waited.error().message};
    if (!waited.value()) {
      co_await execution.DelayFor(50ms);
      continue;
    }
    co_return primitive::ParseAcknowledgement(*waited.value());
  }
}

Task<Outcome> NavigateAnnounceTrash(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string base = arguments.at("base_device_id").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  auto target = ResolveTarget(execution, arguments);
  if (!target) co_return Outcome::Failed(target.error().code, target.error().message);

  navigation_trash::Detector detector;
  std::optional<PrimitiveOperation> announcement;
  std::string announcement_error;
  bool trash_found = false;
  std::string trash_label;
  auto navigation_task = primitive::Run(
      execution, {"base.navigate", "1.0.0"}, base, target.value(), [&]() -> Result<bool> {
        auto snapshot = execution.ReadResource("robots/" + robot + "/detection/head/state");
        if (!snapshot) return Result<bool>::Failure(snapshot.error());
        const auto detected = detector.Observe(snapshot.value());
        if (!detected || announcement) return Result<bool>::Success(true);
        trash_found = true;
        trash_label = navigation_trash::kTrash[*detected].label;
        auto started = execution.StartPrimitive(
            {"speaker.play", "1.0.0"}, speaker,
            {{"uri", navigation_trash::kTrash[*detected].uri}});
        if (started) announcement = std::move(started.value());
        else announcement_error = started.error().message;
        (void)execution.Checkpoint({"trash.found", {}, 0.5, {{"label", trash_label}}});
        return Result<bool>::Success(true);
      });
  const auto navigation = co_await navigation_task;
  if (navigation.status != primitive::Status::kSucceeded)
    co_return PrimitiveFailure("navigation", navigation);

  if (announcement) {
    auto wait_task = WaitForPrimitive(execution, *announcement);
    const auto announced = co_await wait_task;
    if (announced.status != primitive::Status::kSucceeded)
      (void)execution.Checkpoint({"announcement.warning", announced.message, {}, {}});
  } else if (!announcement_error.empty()) {
    (void)execution.Checkpoint({"announcement.warning", announcement_error, {}, {}});
  }
  co_return Outcome::Completed({{"trash_found", trash_found},
                                {"trash_label", trash_label},
                                {"announced", announcement.has_value()},
                                {"destination_reached", true},
                                {"target", target.value()}});
}

Task<Outcome> NavigatePickTrash(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string base = arguments.at("base_device_id").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  auto target = ResolveTarget(execution, arguments);
  if (!target) co_return Outcome::Failed(target.error().code, target.error().message);

  navigation_trash::Detector detector;
  std::optional<std::size_t> detected;
  auto navigation_task = primitive::Run(
      execution, {"base.navigate", "1.0.0"}, base, target.value(), [&]() -> Result<bool> {
        auto snapshot = execution.ReadResource("robots/" + robot + "/detection/head/state");
        if (!snapshot) return Result<bool>::Failure(snapshot.error());
        detected = detector.Observe(snapshot.value());
        if (detected)
          return Result<bool>::Failure({"trash.detected", "interrupt navigation to pick trash"});
        return Result<bool>::Success(true);
      });
  const auto navigation = co_await navigation_task;
  if (!detected) {
    if (navigation.status != primitive::Status::kSucceeded)
      co_return PrimitiveFailure("navigation", navigation);
    co_return Outcome::Completed({{"trash_found", false},
                                  {"picked", false},
                                  {"destination_reached", true},
                                  {"target", target.value()}});
  }

  const auto& trash = navigation_trash::kTrash[*detected];
  (void)execution.Checkpoint({"navigation.interrupted_for_trash", {}, 0.25,
                              {{"label", trash.label}}});
  auto announce_task = primitive::Run(execution, {"speaker.play", "1.0.0"}, speaker,
                                      {{"uri", trash.uri}});
  const auto announced = co_await announce_task;
  if (announced.status != primitive::Status::kSucceeded)
    co_return PrimitiveFailure("announcement", announced);

  auto lie_task = primitive::Run(execution, {"base.posture", "1.0.0"}, base,
                                 {{"posture", "lie"}});
  const auto lied = co_await lie_task;
  if (lied.status != primitive::Status::kSucceeded) co_return PrimitiveFailure("lie", lied);

  Outcome picked;
  try {
    auto pick_task = RunPickTrash(execution);
    picked = co_await pick_task;
  } catch (const std::exception& error) {
    picked = Outcome::Failed("pick.exception", error.what());
  }

  auto stand_task = primitive::Run(execution, {"base.posture", "1.0.0"}, base,
                                    {{"posture", "stand"}});
  const auto stood = co_await stand_task;
  if (stood.status != primitive::Status::kSucceeded) co_return PrimitiveFailure("stand", stood);

  auto resume_task = primitive::Run(execution, {"base.navigate", "1.0.0"}, base,
                                    target.value());
  const auto resumed = co_await resume_task;
  if (resumed.status != primitive::Status::kSucceeded)
    co_return PrimitiveFailure("navigation.resume", resumed);
  if (picked.status != RunResult::Status::kCompleted) co_return picked;
  co_return Outcome::Completed({{"trash_found", true},
                                {"trash_label", trash.label},
                                {"picked", true},
                                {"object_id", picked.result.value("object_id", "")},
                                {"destination_reached", true},
                                {"target", target.value()}});
}

nlohmann::json TargetProperties() {
  return {{"start_waypoint_id", {{"type", "string"}, {"minLength", 1}}},
          {"start_relative_target",
           {{"type", "object"},
            {"properties",
             {{"x_m", {{"type", "number"}}},
              {"y_m", {{"type", "number"}}},
              {"yaw_rad", {{"type", "number"}}}}},
            {"required", {"x_m", "y_m", "yaw_rad"}},
            {"additionalProperties", false}}}};
}

nlohmann::json TargetOneOf() {
  return nlohmann::json::array(
      {{{"required", {"start_waypoint_id"}},
        {"not", {{"required", {"start_relative_target"}}}}},
       {{"required", {"start_relative_target"}},
        {"not", {{"required", {"start_waypoint_id"}}}}}});
}

Definition BaseDefinition(const char* name, const char* description, bool pick) {
  Definition skill;
  skill.metadata.name = name;
  skill.metadata.version = "1.0.0";
  skill.metadata.description = description;
  skill.metadata.library = std::string("island.") + name;
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  auto properties = TargetProperties();
  properties["robot_name"] = {{"type", "string"}, {"minLength", 1}};
  properties["base_device_id"] = {{"type", "string"}, {"minLength", 1}};
  properties["speaker_device_id"] = {{"type", "string"}, {"minLength", 1}};
  if (pick) {
    properties["arm_device_id"] = {{"type", "string"}, {"minLength", 1}};
    properties["gripper_device_id"] = {{"type", "string"}, {"minLength", 1}};
  }
  nlohmann::json required = {"robot_name", "base_device_id", "speaker_device_id"};
  if (pick) {
    required.push_back("arm_device_id");
    required.push_back("gripper_device_id");
  }
  skill.metadata.parameters_schema = {{"type", "object"},
                                      {"properties", std::move(properties)},
                                      {"required", std::move(required)},
                                      {"oneOf", TargetOneOf()},
                                      {"additionalProperties", false}};
  skill.metadata.returns_schema = {{"type", "object"}};
  skill.metadata.resources = {
      {"robots/{robot_name}/detection/head/state", "Head trash detections."},
      {"robots/{robot_name}/base/status", "Localized starting pose."},
      {"robots/{robot_name}/navigation/waypoints/{start_waypoint_id}",
       "Configured absolute map target."}};
  skill.metadata.claim_templates = {
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/base_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/speaker_device_id"}};
  skill.metadata.primitives = {{"base.navigate", "1.0.0"}, {"speaker.play", "1.0.0"}};
  if (pick) {
    skill.metadata.claim_templates.push_back(
        {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/arm_device_id"});
    skill.metadata.claim_templates.push_back(
        {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/gripper_device_id"});
    skill.metadata.primitives.push_back({"base.posture", "1.0.0"});
    skill.metadata.primitives.push_back({"arm.movej", "1.0.0"});
    skill.metadata.primitives.push_back({"arm.movel", "1.0.0"});
    skill.metadata.primitives.push_back({"gripper.motion", "1.0.0"});
    skill.metadata.execution_state_schema = {
        {"type", "object"},
        {"properties",
         {{"phase", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
          {"progress", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
          {"object_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}}},
        {"required", {"phase", "progress"}},
        {"additionalProperties", false}};
  }
  skill.metadata.reentrant = false;
  return skill;
}

}  // namespace

void RegisterSkills(Registry& registry) {
  auto announce = BaseDefinition(
      "navigate_announce_trash",
      "Navigates to a waypoint or relative target while announcing trash without stopping.", false);
  announce.handler = NavigateAnnounceTrash;
  registry.Register(std::move(announce));

  auto pick = BaseDefinition(
      "navigate_pick_trash",
      "Navigates to a waypoint or relative target, picking trash before resuming.", true);
  pick.handler = NavigatePickTrash;
  registry.Register(std::move(pick));
}

}  // namespace island::skills::native::navigate_trash

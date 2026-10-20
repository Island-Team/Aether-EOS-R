#include "share/skills/native/navigate_pick_resume/navigate_pick_resume.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

#include "share/skills/native/common/primitive_support.h"
#include "share/skills/native/pick_trash/pick_trash.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::navigate_pick_resume {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {

constexpr double kMinimumConfidence = 0.40;
constexpr std::size_t kRequiredConsecutiveFrames = 3;

struct DetectionState {
  std::optional<std::uint64_t> sequence;
  std::array<std::size_t, 4> consecutive{};
  std::string label;
  std::string asset;
  double confidence{0.0};
  bool found{false};
};

struct TemporaryNavigationPoint {
  std::string frame_id;
  double x_m;
  double y_m;
  double yaw_rad;

  nlohmann::json Arguments() const {
    return {{"reference", "absolute"},
            {"frame_id", frame_id},
            {"x_m", x_m},
            {"y_m", y_m},
            {"yaw_rad", yaw_rad}};
  }
};

constexpr std::array<const char*, 4> kLabels{{"peel", "empty_box", "bottle", "paper_ball"}};
constexpr std::array<const char*, 4> kAssets{{"asset://head_find_trash/found_peel.wav",
                                               "asset://head_find_trash/found_empty_box.wav",
                                               "asset://head_find_trash/found_bottle.wav",
                                               "asset://head_find_trash/found_paper_ball.wav"}};

bool ObserveTrash(const nlohmann::json& snapshot, DetectionState* state, std::string* error) {
  const auto sequence = snapshot.find("source_sequence");
  const auto detections = snapshot.find("detections");
  if (sequence == snapshot.end() || !sequence->is_number_unsigned() ||
      detections == snapshot.end() || !detections->is_array()) {
    *error = "head detection snapshot is invalid";
    return false;
  }
  const auto current_sequence = sequence->get<std::uint64_t>();
  if (state->sequence && current_sequence <= *state->sequence) return true;
  state->sequence = current_sequence;
  std::array<double, kLabels.size()> confidence{};
  for (const auto& item : *detections) {
    if (!item.is_object() || item.value("kind", "") != "object" ||
        !item.contains("confidence") || !item.at("confidence").is_number())
      continue;
    const double value = item.at("confidence").get<double>();
    if (value < kMinimumConfidence || value > 1.0) continue;
    for (std::size_t index = 0; index < kLabels.size(); ++index) {
      if (item.value("label", "") == kLabels[index]) confidence[index] = std::max(confidence[index], value);
    }
  }
  for (std::size_t index = 0; index < kLabels.size(); ++index) {
    state->consecutive[index] = confidence[index] > 0.0 ? state->consecutive[index] + 1 : 0;
    if (state->consecutive[index] < kRequiredConsecutiveFrames) continue;
    state->found = true;
    state->label = kLabels[index];
    state->asset = kAssets[index];
    state->confidence = confidence[index];
    break;
  }
  return true;
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
    if (!waited) co_return primitive::Result{primitive::Status::kFailed,
                                             waited.error().message};
    if (!waited.value()) {
      co_await execution.DelayFor(50ms);
      continue;
    }
    const auto& acknowledgement = *waited.value();
    const bool accepted = acknowledgement.value("accepted", false);
    const auto result = acknowledgement.value("result", nlohmann::json::object());
    const std::string status = result.value("status", "");
    const std::string reason = result.value("reason", "");
    if (accepted && status == "succeeded")
      co_return primitive::Result{primitive::Status::kSucceeded, {}};
    co_return primitive::Result{
        status == "cancelled" ? primitive::Status::kCancelled : primitive::Status::kFailed,
        reason.empty() ? "primitive execution failed" : reason};
  }
}

std::optional<TemporaryNavigationPoint> CreateTemporaryNavigationPoint(
    const nlohmann::json& status, const nlohmann::json& arguments, std::string* error) {
  if (!status.value("telemetry_valid", false) || !status.value("fresh", false) ||
      !status.value("pose_valid", false) || !status.value("localization_valid", false)) {
    *error = "fresh localized base telemetry is required";
    return std::nullopt;
  }
  if (status.value("frame_id", "") != "map" || !status.contains("pose") ||
      !status.at("pose").is_object()) {
    *error = "base pose must use the map frame";
    return std::nullopt;
  }
  const auto& pose = status.at("pose");
  for (const auto* field : {"x_m", "y_m", "yaw_rad"}) {
    if (!pose.contains(field) || !pose.at(field).is_number() ||
        !std::isfinite(pose.at(field).get<double>())) {
      *error = "base map pose is invalid";
      return std::nullopt;
    }
  }
  const double start_x = pose.at("x_m").get<double>();
  const double start_y = pose.at("y_m").get<double>();
  const double start_yaw = pose.at("yaw_rad").get<double>();
  const double relative_x = arguments.at("x_m").get<double>();
  const double relative_y = arguments.at("y_m").get<double>();
  const double relative_yaw = arguments.at("yaw_rad").get<double>();
  if (!std::isfinite(relative_x) || !std::isfinite(relative_y) ||
      !std::isfinite(relative_yaw)) {
    *error = "relative navigation target is invalid";
    return std::nullopt;
  }
  return TemporaryNavigationPoint{
      .frame_id = "map",
      .x_m = start_x + std::cos(start_yaw) * relative_x - std::sin(start_yaw) * relative_y,
      .y_m = start_y + std::sin(start_yaw) * relative_x + std::cos(start_yaw) * relative_y,
      .yaw_rad = std::remainder(start_yaw + relative_yaw, 2.0 * std::acos(-1.0))};
}

Task<Outcome> NavigatePickResume(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string base = arguments.at("base_device_id").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  const bool announce_only = arguments.value("announce_only", false);
  const std::string detection_uri = "robots/" + robot + "/detection/head/state";
  const std::string base_uri = "robots/" + robot + "/base/status";

  auto status = execution.ReadResource(base_uri);
  if (!status)
    co_return Outcome::Failed("navigation.localization_unavailable",
                              status.error().message);
  std::string point_error;
  const auto temporary_point =
      CreateTemporaryNavigationPoint(status.value(), arguments, &point_error);
  if (!temporary_point)
    co_return Outcome::Failed("navigation.localization_unavailable", point_error);
  const nlohmann::json absolute_target = temporary_point->Arguments();
  (void)execution.Checkpoint({"navigation.temporary_point_created", {}, 0.1, {}});

  DetectionState detection;
  std::optional<PrimitiveOperation> announcement;
  std::string announcement_start_error;
  auto navigation_task = primitive::Run(
      execution, {"base.navigate", "1.0.0"}, base, absolute_target,
      [&]() -> Result<bool> {
        auto snapshot = execution.ReadResource(detection_uri);
        if (!snapshot) return Result<bool>::Failure(snapshot.error());
        std::string error;
        if (!ObserveTrash(snapshot.value(), &detection, &error))
          return Result<bool>::Failure({"detection.invalid", error});
        if (detection.found && announce_only) {
          if (!announcement && announcement_start_error.empty()) {
            auto started = execution.StartPrimitive(
                {"speaker.play", "1.0.0"}, speaker, {{"uri", detection.asset}});
            if (started) announcement = started.value();
            else announcement_start_error = started.error().message;
          }
          return Result<bool>::Success(true);
        }
        if (detection.found)
          return Result<bool>::Failure({"trash.detected", "interrupt navigation to pick trash"});
        return Result<bool>::Success(true);
      });
  const auto navigation = co_await navigation_task;
  if (announce_only) {
    primitive::Result announced{primitive::Status::kSucceeded, {}};
    if (announcement) {
      auto wait_announcement = WaitForPrimitive(execution, *announcement);
      announced = co_await wait_announcement;
    } else if (detection.found) {
      announced = {primitive::Status::kFailed,
                   announcement_start_error.empty() ? "speaker playback was not started"
                                                    : announcement_start_error};
    }
    if (navigation.status != primitive::Status::kSucceeded)
      co_return PrimitiveFailure("navigation", navigation);
    if (announced.status != primitive::Status::kSucceeded)
      co_return PrimitiveFailure("announcement", announced);
    if (!detection.found)
      co_return Outcome::Completed({{"trash_found", false}, {"destination_reached", true}});
    co_return Outcome::Completed({{"trash_found", true},
                                  {"trash_label", detection.label},
                                  {"picked", false},
                                  {"destination_reached", true}});
  }
  if (!detection.found) {
    if (navigation.status != primitive::Status::kSucceeded)
      co_return PrimitiveFailure("navigation", navigation);
    co_return Outcome::Completed({{"trash_found", false}, {"destination_reached", true}});
  }

  (void)execution.Checkpoint({"navigation.interrupted_for_trash", {}, 0.25, {}});
  auto announce_task = primitive::Run(execution, {"speaker.play", "1.0.0"}, speaker,
                                      {{"uri", detection.asset}});
  const auto announced = co_await announce_task;
  if (announced.status != primitive::Status::kSucceeded)
    co_return PrimitiveFailure("announcement", announced);
  auto lie_task = primitive::Run(execution, {"base.posture", "1.0.0"}, base, {{"posture", "lie"}});
  const auto lied = co_await lie_task;
  if (lied.status != primitive::Status::kSucceeded) co_return PrimitiveFailure("lie", lied);

  auto pick_task = RunPickTrash(execution);
  Outcome picked = co_await pick_task;
  auto stand_task = primitive::Run(execution, {"base.posture", "1.0.0"}, base,
                                   {{"posture", "stand"}});
  const auto stood = co_await stand_task;
  if (stood.status != primitive::Status::kSucceeded) co_return PrimitiveFailure("stand", stood);
  if (picked.status != RunResult::Status::kCompleted) co_return picked;

  auto resume_task =
      primitive::Run(execution, {"base.navigate", "1.0.0"}, base, absolute_target);
  const auto resumed = co_await resume_task;
  if (resumed.status != primitive::Status::kSucceeded)
    co_return PrimitiveFailure("navigation.resume", resumed);
  co_return Outcome::Completed({{"trash_found", true},
                                {"trash_label", detection.label},
                                {"picked", true},
                                {"object_id", picked.result.value("object_id", "")},
                                {"destination_reached", true}});
}

}  // namespace

void RegisterSkill(Registry& registry) {
  Definition skill;
  skill.metadata.name = "navigate_pick_resume";
  skill.metadata.version = "1.0.0";
  skill.metadata.description =
      "Navigates while detecting trash, with optional announce-only or pick-and-resume behavior.";
  skill.metadata.library = "island.navigate_pick_resume";
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  skill.metadata.parameters_schema = {
      {"type", "object"},
      {"properties",
       {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
        {"base_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"speaker_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"arm_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"gripper_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"announce_only", {{"type", "boolean"}, {"default", false}}},
        {"x_m", {{"type", "number"}}},
        {"y_m", {{"type", "number"}}},
        {"yaw_rad", {{"type", "number"}}}}},
      {"required", {"robot_name", "base_device_id", "speaker_device_id", "arm_device_id",
                    "gripper_device_id", "x_m", "y_m", "yaw_rad"}},
      {"additionalProperties", false}};
  skill.metadata.returns_schema = {
      {"type", "object"},
      {"properties",
       {{"trash_found", {{"type", "boolean"}}},
         {"trash_label",
          {{"type", "string"}, {"enum", {"peel", "empty_box", "bottle", "paper_ball"}}}},
        {"picked", {{"type", "boolean"}}},
        {"object_id", {{"type", "string"}}},
        {"destination_reached", {{"type", "boolean"}}}}},
      {"required", {"trash_found", "destination_reached"}},
      {"additionalProperties", false}};
  skill.metadata.primitives = {{"base.navigate", "1.0.0"}, {"base.posture", "1.0.0"},
                               {"speaker.play", "1.0.0"}, {"arm.movej", "1.0.0"},
                               {"arm.movel", "1.0.0"}, {"gripper.motion", "1.0.0"}};
  skill.metadata.resources = {{"robots/{robot_name}/base/status", "Localized base pose."},
                              {"robots/{robot_name}/detection/head/state", "Head trash detections."}};
  skill.metadata.claim_templates = {
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/base_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/speaker_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/arm_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/gripper_device_id"}};
  skill.metadata.reentrant = false;
  skill.handler = NavigatePickResume;
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native::navigate_pick_resume

/// Move the arm to the wrist-camera observation pose, announce litter, and reset the arm.
#include "share/skills/native/pick_trash/pick_trash.h"

#include <array>
#include <chrono>
#include <cmath>
#include <string>

#include "share/skills/native/common/primitive_support.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {
using namespace std::chrono_literals;

using PrimitiveResult = island::skills::primitive::Result;
using PrimitiveStatus = island::skills::primitive::Status;

struct LitterClass {
  const char* label;
  const char* announcement;
};

constexpr std::array<LitterClass, 4> kLitterClasses{{
    {"peel", "这是果皮"},
    {"empty_box", "这是空盒子"},
    {"bottle", "这是空瓶"},
    {"paper_ball", "这是纸团"},
}};

const LitterClass* FindClass(const std::string& label) {
  for (const auto& litter : kLitterClasses) {
    if (label == litter.label) return &litter;
  }
  return nullptr;
}

bool ValidVector(const nlohmann::json& value, std::size_t size) {
  if (!value.is_array() || value.size() != size) return false;
  for (const auto& item : value) {
    if (!item.is_number() || !std::isfinite(item.get<double>())) return false;
  }
  return true;
}

const nlohmann::json* BestDetection(const nlohmann::json& snapshot) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return nullptr;
  const nlohmann::json* best = nullptr;
  double confidence = -1.0;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != "object") continue;
    if (FindClass(detection.value("label", "")) == nullptr ||
        !detection.contains("confidence") || !detection.at("confidence").is_number())
      continue;
    const double candidate = detection.at("confidence").get<double>();
    if (!std::isfinite(candidate) || candidate < 0.0 || candidate > 1.0) continue;
    if (candidate > confidence) {
      best = &detection;
      confidence = candidate;
    }
  }
  return best;
}

Outcome PrimitiveFailure(std::string phase, const PrimitiveResult& result) {
  if (result.status == PrimitiveStatus::kCancelled) return Outcome::Cancelled();
  const char* suffix = result.status == PrimitiveStatus::kRejected         ? "rejected"
                       : result.status == PrimitiveStatus::kTimedOut       ? "timed_out"
                       : result.status == PrimitiveStatus::kOutcomeUnknown ? "outcome_unknown"
                                                                             : "failed";
  return result.status == PrimitiveStatus::kRejected
             ? Outcome::Rejected(phase + "." + suffix, result.message)
             : Outcome::Failed(phase + "." + suffix, result.message);
}

Task<Outcome> ObserveTrash(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string arm = arguments.at("arm_device_id").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  const auto observation_hold =
      std::chrono::milliseconds(arguments.value("observation_hold_ms", 2000));
  const auto publish = [&](const char* phase, double progress) {
    (void)execution.PublishState({{"phase", phase}, {"progress", progress}});
  };

  auto joints = execution.GraspObserveJoints();
  if (!joints || !ValidVector(joints.value(), 6))
    co_return Outcome::Failed("observe.invalid_config", "invalid observe joint configuration");
  auto camera = execution.GraspWristCamera();
  if (!camera) co_return Outcome::Failed("observe.invalid_config", camera.error().message);

  publish("starting", 0.05);
  (void)execution.Checkpoint({"trash.observe.started", {}, 0.05, {}});
  auto observe_task = island::skills::primitive::Run(
      execution, {"arm.movej", "1.0.0"}, arm,
      {{"reference", "absolute"}, {"joint_positions_rad", joints.value()}});
  auto observe = co_await observe_task;
  if (observe.status != PrimitiveStatus::kSucceeded)
    co_return PrimitiveFailure("observe", observe);
  publish("observed", 0.35);
  (void)execution.Checkpoint({"trash.observe.completed", {}, 0.35, {}});

  if (observation_hold > 0ms) {
    co_await execution.DelayFor(observation_hold);
    if (execution.cancelled()) co_return Outcome::Cancelled();
  }
  // Require a frame captured after the arm has settled at the observation pose.
  const auto observed_after = std::chrono::steady_clock::now();
  auto snapshot = execution.ObserveObjectsFresh(camera.value(), observed_after);
  if (!snapshot) {
    auto reset_task = island::skills::primitive::Run(
        execution, {"arm.init", "1.0.0"}, arm, nlohmann::json::object());
    auto reset = co_await reset_task;
    if (reset.status != PrimitiveStatus::kSucceeded) co_return PrimitiveFailure("init", reset);
    co_return Outcome::Failed("observe.failed", snapshot.error().message);
  }

  const auto* detection = BestDetection(snapshot.value());
  std::string label;
  std::string announcement = "没有发现垃圾";
  double confidence = 0.0;
  if (detection != nullptr) {
    label = detection->value("label", "");
    announcement = FindClass(label)->announcement;
    confidence = detection->at("confidence").get<double>();
    publish("detected", 0.50);
  } else {
    publish("empty", 0.50);
  }

  auto speak_task = island::skills::primitive::Run(
      execution, {"speaker.speak", "1.0.0"}, speaker,
      {{"text", announcement}, {"language", "zh-CN"}});
  auto speak = co_await speak_task;
  auto reset_task = island::skills::primitive::Run(
      execution, {"arm.init", "1.0.0"}, arm, nlohmann::json::object());
  auto reset = co_await reset_task;
  if (reset.status != PrimitiveStatus::kSucceeded) co_return PrimitiveFailure("init", reset);
  if (speak.status != PrimitiveStatus::kSucceeded) co_return PrimitiveFailure("announce", speak);

  publish("completed", 1.0);
  (void)execution.Checkpoint({"trash.observe.completed", {}, 1.0, {}});
  co_return Outcome::Completed({{"observed", detection != nullptr},
                               {"label", label},
                               {"confidence", confidence}});
}

}  // namespace

Task<Outcome> RunPickTrash(Execution& execution) { return ObserveTrash(execution); }

void RegisterPickTrash(Registry& registry) {
  Definition skill;
  skill.metadata.name = "pick_trash";
  skill.metadata.version = "1.0.0";
  skill.metadata.description =
      "Moves the arm to the wrist-camera observation pose, announces litter, and resets the arm.";
  skill.metadata.library = "island.pick_trash";
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  skill.metadata.parameters_schema = {
      {"type", "object"},
      {"properties", {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
                      {"arm_device_id", {{"type", "string"}, {"minLength", 1}}},
                      {"speaker_device_id", {{"type", "string"}, {"minLength", 1}}},
                      {"observation_hold_ms", {{"type", "integer"}, {"minimum", 0},
                                               {"maximum", 10000}, {"default", 2000}}}}},
      {"required", {"robot_name", "arm_device_id", "speaker_device_id"}},
      {"additionalProperties", false}};
  skill.metadata.returns_schema = {
      {"type", "object"},
      {"properties", {{"observed", {{"type", "boolean"}}},
                      {"label", {{"type", "string"}}},
                      {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
      {"required", {"observed", "label", "confidence"}},
      {"additionalProperties", false}};
  skill.metadata.execution_state_schema = {
      {"type", "object"},
      {"properties", {{"phase", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                      {"progress", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}}},
      {"required", {"phase", "progress"}},
      {"additionalProperties", false}};
  skill.metadata.primitives = {{"arm.init", "1.0.0"},
                                {"arm.movej", "1.0.0"},
                                {"speaker.speak", "1.0.0"}};
  skill.metadata.claim_templates = {
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/arm_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/speaker_device_id"}};
  skill.metadata.reentrant = false;
  skill.handler = RunPickTrash;
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native

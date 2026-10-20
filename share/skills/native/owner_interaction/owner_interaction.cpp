#include "share/skills/native/owner_interaction/owner_interaction.h"

#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "share/skills/native/common/assist_support.h"
#include "share/skills/native/common/navigation_trash_support.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::owner_interaction {
namespace {

using assist::Clock;
using assist::ControlState;
using assist::Json;
using assist::PrimitiveResult;
using assist::PrimitiveStatus;
using assist::Snapshot;
using eos::runtime::skills::builtin::Definition;
using eos::runtime::skills::builtin::Execution;
using eos::runtime::skills::builtin::Outcome;
using eos::runtime::skills::builtin::Task;

enum class Scenario { kWaving, kFallen, kFurniture };

constexpr double kDefaultApproachDistanceM = 1.0;
constexpr double kFallenApproachDistanceM = 0.6;
constexpr double kCenterDeadbandFraction = 0.10;
constexpr double kMinimumOwnerConfidence = 0.4;
constexpr auto kTargetAlignmentTimeout = std::chrono::seconds(60);
constexpr auto kAsrEchoDrain = std::chrono::seconds(1);
constexpr std::string_view kGenericWavingReply = "好的，我知道您需要帮助了";

bool HasOwnerConfidence(const Json& detection) {
  double confidence = 0.0;
  return assist::ReadNumber(detection, "confidence", &confidence) &&
         confidence >= kMinimumOwnerConfidence;
}

bool ValidWaypointTarget(const Json& target) {
  if (!target.is_object()) return false;
  for (const char* key : {"x_m", "y_m", "yaw_rad"}) {
    if (!target.contains(key) || !target.at(key).is_number() ||
        !std::isfinite(target.at(key).get<double>()))
      return false;
  }
  return true;
}

std::string_view WavingReply(std::string_view text) {
  constexpr std::pair<std::string_view, std::string_view> replies[] = {
      {"水", "好的，我来帮您倒水"}, {"垃圾", "好的，我来帮您扔垃圾"},
      {"饭", "好的，我来帮您做饭"}, {"床单", "好的，我来帮您换床单"},
      {"衣服", "好的，我来帮您取衣服"}, {"快递", "好的，我来帮您送快递"}};
  for (const auto& [keyword, reply] : replies) {
    if (text.find(keyword) != std::string_view::npos) return reply;
  }
  return kGenericWavingReply;
}

const char* SkillName(Scenario scenario) {
  if (scenario == Scenario::kWaving) return "interact_waving_owner";
  if (scenario == Scenario::kFallen) return "interact_fallen_owner";
  return "interact_furniture_owner";
}

std::string Attribute(const Json& detection, const char* key) {
  const auto attributes = detection.find("attributes");
  if (attributes == detection.end() || !attributes->is_object()) return {};
  const auto value = attributes->find(key);
  return value != attributes->end() && value->is_string() ? value->get<std::string>()
                                                           : std::string{};
}

std::string RegisteredIdentity(const Json& detection) {
  const std::string name = Attribute(detection, "name");
  return name.empty() || name == "unknown" ? std::string{} : name;
}

bool MatchesScenario(const Json& detection, Scenario scenario) {
  if (scenario == Scenario::kWaving)
    return assist::MatchesAction(detection, "waving", "waving");
  if (scenario == Scenario::kFallen)
    return assist::MatchesAction(detection, "fallen", "fallen");
  const std::string action = Attribute(detection, "action");
  if (action == "waving" || action == "fallen") return false;
  return action == "rest";
}

const Json* FindOwner(const Json& snapshot, Scenario scenario,
                      const std::string& expected_id = {},
                      const std::string& expected_name = {}) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return nullptr;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != "human" ||
        !HasOwnerConfidence(detection) ||
        !MatchesScenario(detection, scenario))
      continue;
    if (!expected_id.empty() && detection.value("id", "") != expected_id) continue;
    const std::string name = RegisteredIdentity(detection);
    if ((!expected_name.empty() && !name.empty() && name != expected_name) ||
        Attribute(detection, "pose").empty())
      continue;
    return &detection;
  }
  return nullptr;
}

const Json* FindTrackedOwner(const Json& snapshot, Scenario scenario,
                              const std::string& expected_id,
                              const std::string& expected_name) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return nullptr;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != "human" ||
        !HasOwnerConfidence(detection) ||
        detection.value("id", "") != expected_id)
      continue;
    const std::string name = RegisteredIdentity(detection);
    if (!expected_name.empty() && !name.empty() && name != expected_name)
      continue;
    return &detection;
  }
  return nullptr;
}

Json Result(const std::string& human_id, const std::string& name, const std::string& pose,
            const std::string& posture_state, const std::string& action,
            const Json& approach_distance, const Json& recognized_text = nullptr,
            const Json& requested_state = nullptr, bool actuation_performed = false,
            const Json& warnings = Json::array()) {
  return {{"human_id", human_id.empty() ? Json(nullptr) : Json(human_id)},
          {"name", name.empty() ? Json(nullptr) : Json(name)},
          {"pose", pose.empty() ? Json(nullptr) : Json(pose)},
          {"posture_state", posture_state.empty() ? Json(nullptr) : Json(posture_state)},
          {"action", action.empty() ? Json(nullptr) : Json(action)},
          {"approach_distance_m", approach_distance},
          {"recognized_text", recognized_text},
          {"requested_state", requested_state},
          {"actuation_performed", actuation_performed},
          {"warnings", warnings}};
}

Outcome Rejected(const char* reason, const std::string& human_id = {},
                 const std::string& name = {}, const std::string& pose = {},
                 const std::string& posture_state = {}, const std::string& action = {}) {
  return {eos::runtime::skills::RunResult::Status::kCompleted,
          false,
          reason,
          Result(human_id, name, pose, posture_state, action, nullptr),
          reason};
}

std::string IdentityAnnouncement(const std::string& name) { return "识别到主人" + name; }

std::string PoseAnnouncement(Scenario scenario, const std::string& pose) {
  const std::string pose_label = pose == "stand" ? "站立" : pose == "sit" ? "坐姿" :
                                 pose == "lie"   ? "躺卧" : "未知";
  if (scenario == Scenario::kWaving)
    return "当前姿态为" + pose_label + "，动作为挥手";
  if (scenario == Scenario::kFallen)
    return "当前姿态为" + pose_label + "，状态为倒地";
  return "当前姿态为" + pose_label + "，状态为休息";
}

Task<Outcome> RunSkill(Execution& execution, Scenario scenario) {
  try {
    const double approach_distance_m =
        scenario == Scenario::kFallen ? kFallenApproachDistanceM : kDefaultApproachDistanceM;
    ControlState control(SkillName(scenario));
    const Json& parameters = execution.arguments();
    const std::string robot_name = parameters.at("robot_name").get<std::string>();
    const std::string base_device_id = parameters.at("base_device_id").get<std::string>();
    const std::string speaker_device_id = parameters.at("speaker_device_id").get<std::string>();
    const bool speech_enabled = parameters.value("speech_enabled", true);
    const bool navigation_enabled = parameters.value("navigation_enabled", true);
    const std::string room_name = parameters.value("room_name", "");
    const std::string arm_device_id =
        scenario == Scenario::kFallen || scenario == Scenario::kFurniture
            ? parameters.at("arm_device_id").get<std::string>()
            : std::string{};
    const bool enhanced_furniture = scenario == Scenario::kFurniture &&
                                    (room_name == "living_room" || room_name == "bedroom");
    const std::string detection_uri = "robots/" + robot_name + "/detection/state";
    const std::string head_detection_uri = "robots/" + robot_name + "/detection/head/state";
    const std::string arm_uri = "robots/" + robot_name + "/arm/state";
    const std::string asr_uri = "robots/" + robot_name + "/world/asr/state";

    assist::Checkpoint(execution, control, "target.acquisition.started", 0.05);
    Snapshot latest;
    std::string error;
    if (!assist::ReadDetection(execution, detection_uri, &latest, &error))
      co_return Outcome::Failed("detection.unavailable", "target acquisition: " + error);

    std::string target_id;
    std::string target_name;
    std::size_t stable_frames = 0;
    std::string pose;
    std::string posture_state;
    std::string action;
    if (const Json* owner = FindOwner(latest.value, scenario);
        owner != nullptr && !owner->value("id", "").empty()) {
      target_id = owner->value("id", "");
      const std::string observed_name = RegisteredIdentity(*owner);
      if (target_name.empty() || !observed_name.empty()) target_name = observed_name;
      pose = Attribute(*owner, "pose");
      posture_state = Attribute(*owner, "posture_state");
      action = Attribute(*owner, "action");
      stable_frames = 1;
    }
    const auto deadline = Clock::now() + assist::kDetectionTimeout;
    while (Clock::now() < deadline && stable_frames < 2) {
      Snapshot fresh;
      auto wait = assist::WaitForDetectionAfter(execution, &control, detection_uri,
                                                 latest.source_sequence, deadline, &fresh, &error);
      if (!co_await wait) {
        if (!control.signal_error.empty()) co_return Outcome::Failed("signal.failed", error);
        if (execution.cancelled()) co_return Outcome::Cancelled();
        if (!error.empty()) co_return Outcome::Failed("detection.unavailable", error);
        break;
      }
      latest = std::move(fresh);
      const Json* owner = FindOwner(latest.value, scenario, target_id, target_name);
      if (owner == nullptr || owner->value("id", "").empty()) {
        target_id.clear();
        target_name.clear();
        stable_frames = 0;
        continue;
      }
      target_id = owner->value("id", "");
      const std::string observed_name = RegisteredIdentity(*owner);
      if (target_name.empty() || !observed_name.empty()) target_name = observed_name;
      pose = Attribute(*owner, "pose");
      posture_state = Attribute(*owner, "posture_state");
      action = Attribute(*owner, "action");
      ++stable_frames;
    }
    if (stable_frames < 2) co_return Rejected("human.owner_not_found");

    double distance_m = 0.0;
    double joint4_rad = 0.0;
    bool aligned = false;
    const auto alignment_deadline = Clock::now() + kTargetAlignmentTimeout;
    while (Clock::now() < alignment_deadline) {
      const Json* owner = FindTrackedOwner(latest.value, scenario, target_id, target_name);
      if (owner != nullptr) {
        if (target_name.empty()) target_name = RegisteredIdentity(*owner);
        const auto centered =
            assist::CenteredInImage(latest.value, *owner, kCenterDeadbandFraction);
        double observed_distance = 0.0;
        if (centered && assist::ReadNumber(*owner, "distance_m", &observed_distance) &&
            observed_distance >= 0.0) {
          distance_m = observed_distance;
          if (*centered) {
            if (distance_m > approach_distance_m &&
                !assist::ReadJoint4(execution, arm_uri, &joint4_rad, &error))
              co_return Outcome::Failed("arm.state_unavailable", "navigation setup: " + error);
            aligned = true;
            break;
          }
        }
      }
      Snapshot fresh;
      error.clear();
      auto wait = assist::WaitForDetectionAfter(execution, &control, detection_uri,
                                                 latest.source_sequence, alignment_deadline,
                                                 &fresh, &error);
      if (!co_await wait) {
        if (!control.signal_error.empty()) co_return Outcome::Failed("signal.failed", error);
        if (execution.cancelled()) co_return Outcome::Cancelled();
        if (!error.empty()) co_return Outcome::Failed("detection.unavailable", error);
        break;
      }
      latest = std::move(fresh);
    }
    if (!aligned)
      co_return Rejected("target.not_aligned", target_id, target_name, pose, posture_state, action);
    assist::Checkpoint(execution, control, "target.acquisition.completed", 0.25);

    PrimitiveResult spoken;
    Json warnings = Json::array();
    bool speech_available = speech_enabled;
    bool identity_announced = !target_name.empty();
    if (speech_enabled) {
      std::vector<std::string> announcements;
      if (!target_name.empty()) announcements.push_back(IdentityAnnouncement(target_name));
      announcements.push_back(PoseAnnouncement(scenario, pose));
      for (const std::string& text : announcements) {
        auto announcement_task =
            assist::Speak(execution, speaker_device_id, text, "zh-CN", &control);
        spoken = co_await announcement_task;
        if (!control.signal_error.empty())
          co_return Outcome::Failed("signal.failed", control.signal_error);
        if (spoken.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
        if (spoken.status != PrimitiveStatus::kSucceeded) {
          if (scenario != Scenario::kFallen)
            co_return Outcome::Failed("announcement.failed", spoken.message);
          warnings.push_back({{"reason", "announcement.failed"}, {"message", spoken.message}});
          speech_available = false;
        }
      }
    }

    const bool bedside_approach =
        enhanced_furniture && room_name == "bedroom" && pose == "lie";
    if (navigation_enabled && bedside_approach) {
      if (!parameters.contains("bedside_waypoint_id"))
        co_return Outcome::InvalidArgument("waypoint.missing", "bedside_waypoint_id is required");
      Json target;
      error.clear();
      if (!assist::ReadJson(execution,
                            "robots/" + robot_name + "/navigation/waypoints/" +
                                parameters.at("bedside_waypoint_id").get<std::string>(),
                            &target, &error))
        co_return Outcome::Failed("waypoint.unavailable", error);
      if (!ValidWaypointTarget(target))
        co_return Outcome::InvalidArgument("waypoint.invalid", "waypoint target is invalid");
      auto navigation_task = assist::CallPrimitive(execution, "base.navigate", "1.0.0",
                                                   base_device_id, target, &control);
      const PrimitiveResult navigation = co_await navigation_task;
      if (!control.signal_error.empty()) co_return Outcome::Failed("signal.failed", control.signal_error);
      if (navigation.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (navigation.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("navigation.failed", navigation.message);
    } else if (navigation_enabled && distance_m > approach_distance_m) {
      const double travel_m = distance_m - approach_distance_m;
      if (travel_m > assist::kMaximumApproachDistanceM)
        co_return Rejected("navigation.distance_exceeded", target_id, target_name, pose,
                           posture_state, action);
      assist::Checkpoint(execution, control, "navigation.started", 0.45);
      const Json relative_target{{"x_m", travel_m * std::cos(joint4_rad)},
                                 {"y_m", travel_m * std::sin(joint4_rad)},
                                 {"yaw_rad", joint4_rad}};
      Json base_status;
      error.clear();
      if (!assist::ReadJson(execution, "robots/" + robot_name + "/base/status", &base_status,
                            &error))
        co_return Outcome::Failed("navigation.localization_unavailable", error);
      auto absolute_target = navigation_trash::AbsoluteMapTarget(base_status, relative_target);
      if (!absolute_target)
        co_return Outcome::Failed("navigation.localization_unavailable",
                                  "fresh localized map pose is required");
      auto navigation_task = assist::NavigateWhileObserving(
          execution, &control, base_device_id, *absolute_target, detection_uri, target_id, &latest,
          scenario == Scenario::kFallen ? assist::HumanCondition::kFallen
                                        : assist::HumanCondition::kAny,
          scenario == Scenario::kFallen ? std::string{} : target_name,
          scenario == Scenario::kFallen ? "fallen" : std::string{}, nullptr, false,
          head_detection_uri, speaker_device_id);
      PrimitiveResult navigation = co_await navigation_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (navigation.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (navigation.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("navigation.failed", navigation.message);
    }
    assist::Checkpoint(execution, control, "navigation.completed", 0.65);

    if (scenario == Scenario::kFallen && !identity_announced) {
      if (const Json* owner = FindTrackedOwner(latest.value, scenario, target_id, target_name)) {
        const std::string observed_name = RegisteredIdentity(*owner);
        if (!observed_name.empty()) target_name = observed_name;
      }
      if (speech_available && !target_name.empty()) {
        auto identity_task = assist::Speak(execution, speaker_device_id,
                                           "识别到主人" + target_name, "zh-CN", &control);
        spoken = co_await identity_task;
        if (!control.signal_error.empty())
          co_return Outcome::Failed("signal.failed", control.signal_error);
        if (spoken.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
        if (spoken.status != PrimitiveStatus::kSucceeded) {
          warnings.push_back(
              {{"reason", "identity_announcement.failed"}, {"message", spoken.message}});
          speech_available = false;
        } else {
          identity_announced = true;
        }
      }
    }

    Json recognized_text = nullptr;
    Json requested_state = nullptr;
    if (speech_enabled && (scenario == Scenario::kWaving || scenario == Scenario::kFurniture)) {
      const std::string prompt = scenario == Scenario::kWaving
                                     ? "您好，有什么可以帮您的"
                                      : "请问你是要打开还是要关闭开关";
      auto prompt_task = assist::Speak(execution, speaker_device_id, prompt, "zh-CN", &control);
      spoken = co_await prompt_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (spoken.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (spoken.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("prompt.failed", spoken.message);

      if (scenario == Scenario::kFurniture && !enhanced_furniture) {
        eos::lib::LogEvent(
            eos::lib::GetLogger("skills.owner_interaction"), spdlog::level::info,
            "owner_interaction_prompted", {{"scenario", "furniture"}},
            "Reached the registered owner and delivered the furniture interaction prompt");
      } else {
        // Discard ASR finalized from the robot's own prompt before listening to the owner.
        co_await execution.DelayFor(kAsrEchoDrain);
        if (execution.cancelled()) co_return Outcome::Cancelled();
        Json baseline;
        error.clear();
        if (!assist::ReadJson(execution, asr_uri, &baseline, &error))
          co_return Outcome::Failed("asr.unavailable", error);
        Json recognized;
        const auto asr_started = Clock::now();
        auto heard = assist::WaitForAsrAfter(
            execution, &control, asr_uri, baseline.value("sequence", std::uint64_t{0}),
            Clock::now() + assist::kAsrTimeout, &recognized, &error);
        if (!co_await heard) {
          if (!control.signal_error.empty()) co_return Outcome::Failed("signal.failed", error);
          if (execution.cancelled()) co_return Outcome::Cancelled();
          if (!error.empty()) co_return Outcome::Failed("asr.unavailable", error);
          eos::lib::LogEvent(eos::lib::GetLogger("skills.owner_interaction"), spdlog::level::warn,
                             "owner_interaction_rejected",
                              {{"scenario", scenario == Scenario::kWaving ? "waving" : "furniture"},
                              {"reason_code", "asr.timeout"},
                              {"wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                              Clock::now() - asr_started)
                                              .count()}},
                             "Interaction rejected because no fresh speech response arrived before "
                             "the 60-second ASR timeout");
          co_return Rejected("asr.timeout", target_id, target_name, pose, posture_state, action);
        }
        const std::string text = recognized.value("text", "");
        recognized_text = text;
        if (scenario == Scenario::kWaving) {
          auto reply_task = assist::Speak(execution, speaker_device_id,
                                          std::string(WavingReply(text)), "zh-CN", &control);
          spoken = co_await reply_task;
          if (!control.signal_error.empty()) co_return Outcome::Failed("signal.failed", control.signal_error);
          if (spoken.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
          if (spoken.status != PrimitiveStatus::kSucceeded) co_return Outcome::Failed("reply.failed", spoken.message);
        } else {
          const auto request = assist::ParseSwitchRequest(text);
          if (request == assist::SwitchRequest::kAmbiguous)
            co_return Rejected("switch.request_ambiguous", target_id, target_name, pose,
                               posture_state, action);
          requested_state = assist::SwitchRequestName(request);
        }
      }
    } else if (speech_available) {
      auto assistance_task = assist::Speak(execution, speaker_device_id,
                                           "请不要担心，我来帮助您", "zh-CN", &control);
      spoken = co_await assistance_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (spoken.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (spoken.status != PrimitiveStatus::kSucceeded) {
        warnings.push_back(
            {{"reason", "assistance_announcement.failed"}, {"message", spoken.message}});
      }
    }

    bool actuation_performed = false;
    if (enhanced_furniture && !requested_state.is_null()) {
      if (navigation_enabled) {
        const std::string waypoint_id = parameters.at("switch_waypoint_id").get<std::string>();
        Json target;
        error.clear();
        if (!assist::ReadJson(
                execution, "robots/" + robot_name + "/navigation/waypoints/" + waypoint_id,
                &target, &error))
          co_return Outcome::Failed("waypoint.unavailable", error);
        if (!ValidWaypointTarget(target))
          co_return Outcome::InvalidArgument("waypoint.invalid", "waypoint target is invalid");
        auto navigation_task = assist::CallPrimitive(execution, "base.navigate", "1.0.0",
                                                     base_device_id, target, &control);
        PrimitiveResult navigation = co_await navigation_task;
        if (navigation.status != PrimitiveStatus::kSucceeded)
          co_return navigation.status == PrimitiveStatus::kCancelled
                        ? Outcome::Cancelled()
                        : Outcome::Failed("navigation.failed", navigation.message);
      }
      auto move_task = assist::CallPrimitive(
          execution, "arm.movej", "1.0.0", arm_device_id,
          {{"reference", "absolute"},
           {"joint_positions_rad", parameters.at("switch_joint_positions_rad")}},
          &control);
      PrimitiveResult moved = co_await move_task;
      if (moved.status != PrimitiveStatus::kSucceeded)
        co_return moved.status == PrimitiveStatus::kCancelled
                      ? Outcome::Cancelled()
                      : Outcome::Failed("switch.actuation_failed", moved.message);
      actuation_performed = true;
      auto announce_task = assist::Speak(
          execution, speaker_device_id,
          requested_state == "on" ? "我帮您打开开关" : "我帮您关闭开关", "zh-CN", &control);
      spoken = co_await announce_task;
      if (spoken.status != PrimitiveStatus::kSucceeded)
        co_return spoken.status == PrimitiveStatus::kCancelled
                      ? Outcome::Cancelled()
                      : Outcome::Failed("switch.announcement_failed", spoken.message);
      auto reset_task = assist::CallPrimitive(execution, "arm.init", "1.0.0", arm_device_id,
                                              Json::object(), &control);
      PrimitiveResult reset = co_await reset_task;
      if (reset.status != PrimitiveStatus::kSucceeded)
        co_return reset.status == PrimitiveStatus::kCancelled
                      ? Outcome::Cancelled()
                      : Outcome::Failed("switch.arm_reset_failed", reset.message);
    }
    if (scenario == Scenario::kFallen) {
      assist::Checkpoint(execution, control, "assistance_arm.started", 0.85);
      auto assistance_task =
          assist::CallPrimitive(execution, "arm.movej", "1.0.0", arm_device_id,
                                {{"reference", "absolute"},
                                 {"joint_positions_rad", parameters.at("assistance_joint_positions_rad")}},
                                &control);
      const PrimitiveResult assistance = co_await assistance_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (assistance.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (assistance.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("assistance_arm.failed", assistance.message);
      assist::Checkpoint(execution, control, "assistance_arm_reset.started", 0.92);
      auto reset_task = assist::CallPrimitive(
          execution, "arm.movej", "1.0.0", arm_device_id,
          {{"reference", "absolute"},
           {"joint_positions_rad", parameters.at("reset_joint_positions_rad")}},
          &control);
      const PrimitiveResult reset = co_await reset_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (reset.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (reset.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("assistance_arm_reset.failed", reset.message);
      assist::Checkpoint(execution, control, "assistance_arm_init.started", 0.96);
      auto init_task = assist::CallPrimitive(execution, "arm.init", "1.0.0", arm_device_id,
                                             Json::object(), &control);
      const PrimitiveResult initialized = co_await init_task;
      if (!control.signal_error.empty())
        co_return Outcome::Failed("signal.failed", control.signal_error);
      if (initialized.status == PrimitiveStatus::kCancelled) co_return Outcome::Cancelled();
      if (initialized.status != PrimitiveStatus::kSucceeded)
        co_return Outcome::Failed("assistance_arm_init.failed", initialized.message);
      actuation_performed = true;
      assist::Checkpoint(execution, control, "assistance_arm.completed", 0.98);
    }

    assist::Checkpoint(execution, control, "interaction.completed", 1.0, "completed");
    Json result = Result(target_id, target_name, pose, posture_state, action, approach_distance_m,
                         recognized_text, requested_state, actuation_performed, warnings);
    const char* scenario_name = scenario == Scenario::kWaving   ? "waving"
                                : scenario == Scenario::kFallen ? "fallen"
                                                                 : "furniture";
    eos::lib::LogEvent(
        eos::lib::GetLogger("skills.owner_interaction"), spdlog::level::info,
        "owner_interaction_completed",
        {{"scenario", scenario_name},
         {"human_id", target_id},
         {"name", target_name},
         {"pose", pose},
         {"posture_state", posture_state},
         {"action", action},
         {"approach_distance_m", approach_distance_m},
         {"actuation_performed", actuation_performed},
         {"warnings", warnings}},
        std::string("Owner interaction completed with final scenario ") + scenario_name);
    co_return Outcome::Completed(std::move(result));
  } catch (...) {
    co_return Outcome::Failed("skill.exception",
                              std::string(SkillName(scenario)) + " failed unexpectedly");
  }
}

constexpr const char* kParametersSchema = R"json({
  "$schema":"https://json-schema.org/draft/2020-12/schema",
  "type":"object",
  "properties":{
    "robot_name":{"type":"string","minLength":1},
    "base_device_id":{"type":"string","minLength":1},
    "speaker_device_id":{"type":"string","minLength":1},
    "room_name":{"type":"string","enum":["living_room","kitchen","bedroom","dining_room"]},
    "bedside_waypoint_id":{"type":"string","minLength":1},
    "switch_waypoint_id":{"type":"string","minLength":1},
    "switch_joint_positions_rad":{"type":"array","minItems":6,"maxItems":6,"items":{"type":"number"}},
    "assistance_joint_positions_rad":{"type":"array","minItems":6,"maxItems":6,"items":{"type":"number"}},
    "reset_joint_positions_rad":{"type":"array","minItems":6,"maxItems":6,"items":{"type":"number"}},
    "navigation_enabled":{"type":"boolean","default":true},
    "speech_enabled":{"type":"boolean","default":true}
  },
  "required":["robot_name","base_device_id","speaker_device_id"],
  "additionalProperties":false
})json";

constexpr const char* kReturnsSchema = R"json({
  "$schema":"https://json-schema.org/draft/2020-12/schema",
  "type":"object",
  "properties":{
    "human_id":{"type":["string","null"]},
    "name":{"type":["string","null"]},
    "pose":{"type":["string","null"]},
    "posture_state":{"type":["string","null"]},
    "action":{"type":["string","null"]},
    "approach_distance_m":{"type":["number","null"]},
    "recognized_text":{"type":["string","null"]},
    "requested_state":{"type":["string","null"],"enum":["on","off",null]},
    "actuation_performed":{"type":"boolean"},
    "warnings":{"type":"array","items":{"type":"object","properties":{
      "reason":{"type":"string","minLength":1},
      "message":{"type":"string"}
    },"required":["reason","message"],"additionalProperties":false}}
  },
  "required":["human_id","name","pose","posture_state","action","approach_distance_m",
              "recognized_text","requested_state","actuation_performed","warnings"],
  "additionalProperties":false
})json";

constexpr const char* kCommandSchema = R"json({
  "$schema":"https://json-schema.org/draft/2020-12/schema",
  "type":"object",
  "properties":{
    "command":{"type":"string","enum":["pause","resume"]},
    "source_execution_id":{"type":"string","minLength":1},
    "correlation_id":{"type":"string","minLength":1}
  },
  "required":["command"],
  "additionalProperties":false
})json";

Json EventSchema(const char* skill_name) {
  return {{"$schema", "https://json-schema.org/draft/2020-12/schema"},
          {"type", "object"},
          {"properties",
           {{"skill", {{"const", skill_name}}},
            {"phase", {{"type", "string"}, {"minLength", 1}}},
            {"state", {{"type", "string"},
                       {"enum", {"started", "progress", "paused", "resumed", "completed"}}}},
            {"progress", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
            {"source_execution_id", {{"type", "string"}, {"minLength", 1}}},
            {"correlation_id", {{"type", "string"}, {"minLength", 1}}}}},
          {"required", {"skill", "phase", "state", "progress"}},
          {"additionalProperties", false}};
}

void Register(Scenario scenario, eos::runtime::skills::builtin::Registry& registry) {
  const char* name = SkillName(scenario);
  Definition skill;
  skill.metadata.name = name;
  skill.metadata.version = "1.0.0";
  skill.metadata.description = "Interacts with a registered owner using one frozen approach target.";
  skill.metadata.library = std::string("island.") + name;
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  Json parameters_schema = Json::parse(kParametersSchema);
  if (scenario == Scenario::kFallen || scenario == Scenario::kFurniture) {
    parameters_schema["properties"]["arm_device_id"] = {{"type", "string"}, {"minLength", 1}};
    parameters_schema["required"].push_back("arm_device_id");
  }
  if (scenario == Scenario::kFallen) {
    parameters_schema["required"].push_back("assistance_joint_positions_rad");
    parameters_schema["required"].push_back("reset_joint_positions_rad");
  }
  if (scenario == Scenario::kFurniture) {
    parameters_schema["required"].push_back("switch_waypoint_id");
    parameters_schema["required"].push_back("switch_joint_positions_rad");
  }
  skill.metadata.parameters_schema = std::move(parameters_schema);
  skill.metadata.returns_schema = Json::parse(kReturnsSchema);
  skill.metadata.primitives = {{"speaker.speak", "1.0.0"},
                               {"speaker.play", "1.0.0"},
                               {"base.navigate", "1.0.0"}};
  if (scenario == Scenario::kFallen || scenario == Scenario::kFurniture)
    skill.metadata.primitives.push_back({"arm.movej", "1.0.0"});
  if (scenario == Scenario::kFallen || scenario == Scenario::kFurniture)
    skill.metadata.primitives.push_back({"arm.init", "1.0.0"});
  skill.metadata.claim_templates = {
      {eos::runtime::skills::ClaimKind::kDevice,
       eos::runtime::skills::ClaimMode::kExclusiveUse, "/base_device_id"},
      {eos::runtime::skills::ClaimKind::kDevice,
       eos::runtime::skills::ClaimMode::kExclusiveUse, "/speaker_device_id"}};
  if (scenario == Scenario::kFallen || scenario == Scenario::kFurniture)
    skill.metadata.claim_templates.push_back(
        {eos::runtime::skills::ClaimKind::kDevice,
         eos::runtime::skills::ClaimMode::kExclusiveUse, "/arm_device_id"});
  skill.metadata.resources = {
      {"robots/{robot_name}/detection/state", "Fresh semantic human detections."},
      {"robots/{robot_name}/detection/head/state", "Fresh head-camera trash detections."},
      {"robots/{robot_name}/base/status", "Fresh localized base pose."},
       {"robots/{robot_name}/arm/state", "Latest measured manipulator joint state."}};
  if (scenario == Scenario::kFurniture) {
    skill.metadata.resources.push_back(
        {"robots/{robot_name}/navigation/waypoints/{id}", "Configured furniture waypoint."});
  }
  if (scenario != Scenario::kFallen)
    skill.metadata.resources.push_back(
        {"robots/{robot_name}/world/asr/state", "Fresh speech transcription."});
  skill.metadata.accepted_signals.push_back({"skill.command", Json::parse(kCommandSchema)});
  skill.metadata.emitted_signals.push_back({"skill.event", EventSchema(name)});
  skill.metadata.supports_signals = true;
  skill.metadata.reentrant = true;
  skill.handler = [scenario](Execution& execution) { return RunSkill(execution, scenario); };
  registry.Register(std::move(skill));
}

}  // namespace

void RegisterSkills(eos::runtime::skills::builtin::Registry& registry) {
  Register(Scenario::kWaving, registry);
  Register(Scenario::kFallen, registry);
  Register(Scenario::kFurniture, registry);
}

}  // namespace island::skills::native::owner_interaction

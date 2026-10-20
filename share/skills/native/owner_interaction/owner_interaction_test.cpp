#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "share/skills/native/skills.h"
#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
namespace {

using namespace eos::runtime::skills;
using namespace eos::runtime::skills::builtin;
using Json = nlohmann::json;

Json OwnerDetection(std::uint64_t sequence, std::string action = "waving",
                      std::string pose = "stand", std::string name = "小明",
                      Json confidence = 0.95, Json bbox = {220, 80, 420, 420},
                      double distance_m = 2.0) {
  Json attributes{{"pose", std::move(pose)}, {"action", std::move(action)}};
  if (!name.empty()) attributes["name"] = std::move(name);
  return {{"source_sequence", sequence},
          {"image_width_px", 640},
          {"image_height_px", 480},
          {"detections",
            {{{"id", "human:7"},
              {"kind", "human"},
               {"confidence", std::move(confidence)},
               {"bbox", std::move(bbox)},
             {"distance_m", distance_m},
             {"attributes", std::move(attributes)}}}}};
}

ExecutionHooks Hooks(std::function<bool()> cancelled = [] { return false; }) {
  ExecutionHooks hooks;
  hooks.cancelled = std::move(cancelled);
  hooks.receive_signal = [](std::chrono::milliseconds) { return std::optional<Signal>{}; };
  hooks.emit_signal = [](const Signal&) { return true; };
  hooks.emit_checkpoint = [](const Checkpoint&) { return true; };
  return hooks;
}

struct InteractionCapture {
  std::mutex mutex;
  std::vector<std::pair<std::string, Json>> primitives;
  std::vector<InferenceRequest> inferences;
  std::atomic<std::uint64_t> detection_sequence{0};
  std::atomic<std::uint64_t> asr_reads{0};
  std::atomic<std::size_t> asr_baseline_primitive_count{0};
  std::atomic<std::uint64_t> waypoint_reads{0};
  std::string action{"waving"};
  std::string pose{"stand"};
  double distance_m{2.0};
  std::string asr_text{"请告诉我天气"};
  std::function<Json(std::uint64_t)> confidence_for_sequence =
      [](std::uint64_t) { return Json(0.95); };
  std::function<Json(std::uint64_t)> bbox_for_sequence =
      [](std::uint64_t) { return Json{220, 80, 420, 420}; };
  std::function<std::string(std::uint64_t)> name_for_sequence =
      [](std::uint64_t) { return "小明"; };
  std::function<Json(std::string_view, std::size_t)> primitive_result =
      [](std::string_view, std::size_t) {
        return Json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
};

RuntimeCapabilities Capabilities(InteractionCapture* capture, Json inference_response,
                                  std::function<void(Cancelled)> inference_wait = {},
                                  bool inference_failure = false) {
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [capture](const std::string& uri) {
    if (uri.ends_with("/detection/head/state"))
      return Json{{"source_sequence", ++capture->detection_sequence}, {"detections", Json::array()}};
    if (uri.ends_with("/detection/state")) {
      const auto sequence = ++capture->detection_sequence;
      return OwnerDetection(sequence, capture->action, capture->pose,
                            capture->name_for_sequence(sequence),
                            capture->confidence_for_sequence(sequence),
                            capture->bbox_for_sequence(sequence), capture->distance_m);
    }
    if (uri.ends_with("/base/status"))
      return Json{{"telemetry_valid", true},
                  {"fresh", true},
                  {"pose_valid", true},
                  {"localization_valid", true},
                  {"frame_id", "map"},
                  {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    if (uri.ends_with("/arm/state"))
      return Json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                   {"joint_valid", {true, true, true, true}}};
    if (uri.find("/navigation/waypoints/") != std::string::npos) {
      ++capture->waypoint_reads;
      return Json{{"reference", "absolute"}, {"frame_id", "map"},
                  {"x_m", 3.0}, {"y_m", 4.0}, {"yaw_rad", 0.5}};
    }
    const auto read = ++capture->asr_reads;
    if (read == 1) {
      std::lock_guard lock(capture->mutex);
      capture->asr_baseline_primitive_count = capture->primitives.size();
    }
    return Json{{"sequence", read == 1 ? 10 : 11},
                {"text", read == 1 ? "旧文本" : capture->asr_text}};
  };
  capabilities.execute_primitive =
      [capture](const PrimitiveReference& primitive, const std::string&, Json arguments,
                const std::string&, Cancelled) {
        std::lock_guard lock(capture->mutex);
        capture->primitives.emplace_back(primitive.name, std::move(arguments));
        return capture->primitive_result(primitive.name, capture->primitives.size());
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.infer_once =
      [capture, response = std::move(inference_response), wait = std::move(inference_wait),
       inference_failure](
          const InferenceRequest& request, Cancelled cancelled) {
        {
          std::lock_guard lock(capture->mutex);
          capture->inferences.push_back(request);
        }
        if (inference_failure) throw std::runtime_error("provider unavailable");
        if (wait) wait(std::move(cancelled));
        return InferenceResult{request.authority, request.source, request.source_sequence,
                               request.source_generation, response};
      };
  return capabilities;
}

class OwnerInteractionTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

class WavingRequestTest : public OwnerInteractionTest,
                          public testing::WithParamInterface<std::pair<std::string, std::string>> {};

TEST_P(WavingRequestTest, RepliesWithFixedIntentWithoutInference) {
  InteractionCapture capture;
  capture.asr_text = GetParam().first;
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_waving_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"room_name", "living_room"}},
      "waving-intent", Hooks());
  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(capture.inferences.empty());
  ASSERT_GE(capture.primitives.size(), 4u);
  EXPECT_EQ(capture.primitives.at(0).second.at("text"), "识别到主人小明");
  EXPECT_EQ(capture.primitives.at(1).second.at("text"), "当前姿态为站立，动作为挥手");
  EXPECT_EQ(capture.primitives.at(capture.primitives.size() - 2).second.at("text"),
            "您好，有什么可以帮您的");
  EXPECT_GE(capture.asr_baseline_primitive_count, 3u);
  EXPECT_EQ(capture.primitives.back().second.at("text"), GetParam().second);
  EXPECT_EQ(result.result.at("recognized_text"), GetParam().first);
}

INSTANTIATE_TEST_SUITE_P(
    CommonPhrasings, WavingRequestTest,
    testing::Values(
        std::pair{"请帮我拿水。", "好的，我来帮您倒水"},
        std::pair{"帮我扔垃圾", "好的，我来帮您扔垃圾"},
        std::pair{"我要做饭？", "好的，我来帮您做饭"},
        std::pair{"帮我换床单", "好的，我来帮您换床单"},
        std::pair{"帮我取衣服", "好的，我来帮您取衣服"},
        std::pair{"帮我送快递", "好的，我来帮您送快递"},
        std::pair{"检查阳台的窗户", "好的，我知道您需要帮助了"}));

TEST_F(OwnerInteractionTest, UnusableWavingRequestUsesSafeReplyWithoutInferenceByDefault) {
  InteractionCapture capture;
  capture.asr_text = "。。。！？";
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_waving_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}}, "waving-fallback-disabled", Hooks());
  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(capture.inferences.empty());
  EXPECT_EQ(capture.primitives.back().second.at("text"), "好的，我知道您需要帮助了");
  EXPECT_TRUE(result.result.at("warnings").empty());
}

TEST_F(OwnerInteractionTest, IgnoresMissingAndLowOwnerConfidenceDuringSelection) {
  for (const Json confidence : {Json(nullptr), Json(0.39)}) {
    InteractionCapture capture;
    capture.confidence_for_sequence =
        [confidence](std::uint64_t sequence) { return sequence == 1 ? confidence : Json(0.95); };
    Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
    RegisterSkills(registry);
    const RunResult result = registry.ExecuteWithHooks(
        "interact_waving_owner", "1.0.0",
        {{"robot_name", "robot"}, {"base_device_id", "base-0"},
         {"speaker_device_id", "speaker-0"}}, "low-confidence", Hooks());
    EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
    EXPECT_GE(capture.detection_sequence, 3u);
  }
}

TEST_F(OwnerInteractionTest, LowConfidenceFrameCannotTrackConfirmedOwner) {
  InteractionCapture capture;
  capture.confidence_for_sequence =
       [](std::uint64_t sequence) { return Json(sequence == 3 ? 0.39 : 0.95); };
  capture.bbox_for_sequence = [](std::uint64_t sequence) {
    return sequence == 2 ? Json{0, 80, 100, 420} : Json{220, 80, 420, 420};
  };
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_waving_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}}, "tracking-confidence", Hooks());
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_GE(capture.detection_sequence, 4u);
  EXPECT_FALSE(capture.primitives.empty());
}

Json AssistancePose() { return Json::array({0.1, 0.2, 0.3, 0.4, 0.5, 0.6}); }
Json ResetPose() { return Json::array({0.0, 0.1, 0.2, 0.3, 0.4, 0.5}); }
Json SwitchPose() { return Json::array({0.2, 0.3, 0.4, 0.5, 0.6, 0.7}); }

TEST_F(OwnerInteractionTest, LivingRoomFurnitureParsesAndActuatesSwitchRequest) {
  InteractionCapture capture;
  capture.action = "rest";
  capture.pose = "sit";
  capture.asr_text = "请打开";
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"room_name", "living_room"}, {"switch_waypoint_id", "switch"},
       {"switch_joint_positions_rad", SwitchPose()}}, "living-switch", Hooks());
  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(result.result.at("requested_state"), "on");
  EXPECT_TRUE(result.result.at("actuation_performed"));
  EXPECT_TRUE(capture.inferences.empty());
  ASSERT_EQ(capture.primitives.size(), 8u);
  EXPECT_EQ(capture.primitives.at(0).second.at("text"), "识别到主人小明");
  EXPECT_EQ(capture.primitives.at(1).second.at("text"), "当前姿态为坐姿，状态为休息");
  EXPECT_EQ(capture.primitives.at(3).second.at("text"), "请问你是要打开还是要关闭开关");
  EXPECT_EQ(capture.primitives.at(4).first, "base.navigate");
  EXPECT_EQ(capture.primitives.at(5).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(5).second.at("joint_positions_rad"),
            SwitchPose());
  EXPECT_EQ(capture.primitives.at(6).second.at("text"), "我帮您打开开关");
  EXPECT_EQ(capture.primitives.at(7).first, "arm.init");
}

TEST_F(OwnerInteractionTest, BedroomLyingOwnerUsesBedsideThenSwitchWaypoint) {
  InteractionCapture capture;
  capture.action = "rest";
  capture.pose = "lie";
  capture.asr_text = "关闭";
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"room_name", "bedroom"}, {"switch_waypoint_id", "switch"},
       {"switch_joint_positions_rad", SwitchPose()}, {"bedside_waypoint_id", "bedside"}}, "bedroom-switch", Hooks());
  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(result.result.at("requested_state"), "off");
  ASSERT_EQ(capture.primitives.size(), 8u);
  EXPECT_EQ(capture.primitives.at(2).first, "base.navigate");
  EXPECT_DOUBLE_EQ(capture.primitives.at(2).second.at("x_m"), 3.0);
  EXPECT_EQ(capture.primitives.at(4).first, "base.navigate");
  EXPECT_EQ(capture.primitives.at(5).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(5).second.at("joint_positions_rad"),
            SwitchPose());
  EXPECT_EQ(capture.primitives.at(6).second.at("text"), "我帮您关闭开关");
}

TEST_F(OwnerInteractionTest, BedroomLyingOwnerRehearsesSwitchWithoutNavigation) {
  InteractionCapture capture;
  capture.action = "rest";
  capture.pose = "lie";
  capture.distance_m = 1.0;
  capture.asr_text = "关闭";
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"room_name", "bedroom"}, {"switch_waypoint_id", "switch"},
       {"switch_joint_positions_rad", SwitchPose()}, {"bedside_waypoint_id", "bedside"}, {"navigation_enabled", false}},
      "bedroom-switch-rehearsal", Hooks());

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.result.at("requested_state"), "off");
  EXPECT_TRUE(result.result.at("actuation_performed"));
  EXPECT_EQ(capture.waypoint_reads, 0u);
  ASSERT_EQ(capture.primitives.size(), 6u);
  EXPECT_TRUE(std::none_of(capture.primitives.begin(), capture.primitives.end(),
                           [](const auto& call) { return call.first == "base.navigate"; }));
  EXPECT_EQ(capture.primitives.at(3).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(4).second.at("text"), "我帮您关闭开关");
  EXPECT_EQ(capture.primitives.at(5).first, "arm.init");
}

TEST_F(OwnerInteractionTest, AmbiguousFurnitureRequestDoesNotActuate) {
  InteractionCapture capture;
  capture.action = "rest";
  capture.asr_text = "随便";
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"room_name", "living_room"}, {"switch_waypoint_id", "switch"},
       {"switch_joint_positions_rad", SwitchPose()}}, "ambiguous-switch", Hooks());
  EXPECT_EQ(result.reason_code, "switch.request_ambiguous");
  EXPECT_EQ(capture.primitives.size(), 4u);
}

TEST_F(OwnerInteractionTest, FallenAndFurnitureRemainInferenceFree) {
  const auto run = [&](const char* skill, std::string action, std::string pose) {
    InteractionCapture capture;
    capture.action = std::move(action);
    capture.pose = std::move(pose);
    Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
    RegisterSkills(registry);
    Json arguments{{"robot_name", "robot"},
                   {"base_device_id", "base-0"},
                   {"speaker_device_id", "speaker-0"},
                   {"speech_enabled", false}};
    const bool fallen = std::string_view(skill) == "interact_fallen_owner";
    if (fallen || std::string_view(skill) == "interact_furniture_owner")
      arguments["arm_device_id"] = "arm-0";
    if (fallen) {
      arguments["assistance_joint_positions_rad"] = AssistancePose();
      arguments["reset_joint_positions_rad"] = ResetPose();
    } else if (std::string_view(skill) == "interact_furniture_owner") {
      arguments["switch_waypoint_id"] = "switch";
      arguments["switch_joint_positions_rad"] = SwitchPose();
    }
    const RunResult result = registry.ExecuteWithHooks(
        skill, "1.0.0", arguments, std::string(skill) + "-isolation", Hooks());
    EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
    EXPECT_TRUE(capture.inferences.empty());
    ASSERT_EQ(capture.primitives.size(), fallen ? 4u : 1u);
    EXPECT_EQ(capture.primitives.front().first, "base.navigate");
    if (fallen) {
      EXPECT_EQ(capture.primitives.at(1).first, "arm.movej");
      EXPECT_EQ(capture.primitives.at(1).second.at("joint_positions_rad"),
                AssistancePose());
      EXPECT_EQ(capture.primitives.at(2).first, "arm.movej");
      EXPECT_EQ(capture.primitives.at(2).second.at("joint_positions_rad"),
                 ResetPose());
      EXPECT_EQ(capture.primitives.at(3).first, "arm.init");
      EXPECT_TRUE(capture.primitives.at(3).second.empty());
    }
  };

  run("interact_fallen_owner", "fallen", "lie");
  run("interact_furniture_owner", "rest", "lie");
  run("interact_furniture_owner", "rest", "sit");
}

TEST_F(OwnerInteractionTest, UnidentifiedFallenOwnerIsAssistedAfterPostureAnnouncement) {
  InteractionCapture capture;
  capture.action = "fallen";
  capture.pose = "lie";
  capture.name_for_sequence = [](std::uint64_t) { return std::string{}; };
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);

  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0",
      {{"robot_name", "robot"},
       {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"},
       {"arm_device_id", "arm-0"},
       {"assistance_joint_positions_rad", AssistancePose()},
       {"reset_joint_positions_rad", ResetPose()}},
      "fallen-without-name", Hooks());

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  ASSERT_EQ(capture.primitives.size(), 6u);
  EXPECT_EQ(capture.primitives.at(0).first, "speaker.speak");
  EXPECT_EQ(capture.primitives.at(0).second.at("text"), "当前姿态为躺卧，状态为倒地");
  EXPECT_EQ(capture.primitives.at(1).first, "base.navigate");
  EXPECT_DOUBLE_EQ(capture.primitives.at(1).second.at("x_m"), 1.4);
  EXPECT_EQ(capture.primitives.at(2).second.at("text"), "请不要担心，我来帮助您");
  EXPECT_EQ(capture.primitives.at(3).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(4).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(5).first, "arm.init");
  EXPECT_DOUBLE_EQ(result.result.at("approach_distance_m"), 0.6);
  EXPECT_TRUE(result.result.at("name").is_null());
}

TEST_F(OwnerInteractionTest, AnnouncesFallenIdentityAfterApproachDiscoversIt) {
  InteractionCapture capture;
  capture.action = "fallen";
  capture.pose = "lie";
  capture.name_for_sequence =
      [](std::uint64_t sequence) { return sequence >= 3 ? "小明" : std::string{}; };
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);

  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0",
      {{"robot_name", "robot"},
       {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"},
       {"arm_device_id", "arm-0"},
       {"assistance_joint_positions_rad", AssistancePose()},
       {"reset_joint_positions_rad", ResetPose()}},
      "fallen-delayed-name", Hooks());

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  ASSERT_EQ(capture.primitives.size(), 7u);
  EXPECT_EQ(capture.primitives.at(0).second.at("text"), "当前姿态为躺卧，状态为倒地");
  EXPECT_EQ(capture.primitives.at(1).first, "base.navigate");
  EXPECT_EQ(capture.primitives.at(2).second.at("text"), "识别到主人小明");
  EXPECT_EQ(capture.primitives.at(3).second.at("text"), "请不要担心，我来帮助您");
  EXPECT_EQ(capture.primitives.at(4).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(5).first, "arm.movej");
  EXPECT_EQ(capture.primitives.at(6).first, "arm.init");
  EXPECT_EQ(result.result.at("name"), "小明");
}

TEST_F(OwnerInteractionTest, FallenFinalArmInitFailureStopsCompletion) {
  InteractionCapture capture;
  capture.action = "fallen";
  capture.pose = "lie";
  capture.primitive_result = [](std::string_view primitive, std::size_t) {
    const char* status = primitive == "arm.init" ? "failed" : "succeeded";
    return Json{{"accepted", true},
                {"result", {{"status", status}, {"message", "reset failed"}}}};
  };
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);

  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"assistance_joint_positions_rad", AssistancePose()},
       {"reset_joint_positions_rad", ResetPose()},
       {"speech_enabled", false}},
      "fallen-init-failure", Hooks());

  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "assistance_arm_init.failed");
  ASSERT_EQ(capture.primitives.size(), 4u);
  EXPECT_EQ(capture.primitives.back().first, "arm.init");
}

TEST_F(OwnerInteractionTest, FallenFinalArmInitCancellationCancelsSkill) {
  InteractionCapture capture;
  capture.action = "fallen";
  capture.pose = "lie";
  capture.primitive_result = [](std::string_view primitive, std::size_t) {
    const char* status = primitive == "arm.init" ? "cancelled" : "succeeded";
    return Json{{"accepted", true}, {"result", {{"status", status}}}};
  };
  Registry registry(timer_, Capabilities(&capture, {{"text", "unused"}}));
  RegisterSkills(registry);

  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0",
      {{"robot_name", "robot"}, {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"}, {"arm_device_id", "arm-0"},
       {"assistance_joint_positions_rad", AssistancePose()},
       {"reset_joint_positions_rad", ResetPose()},
       {"speech_enabled", false}},
      "fallen-init-cancel", Hooks());

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  ASSERT_EQ(capture.primitives.size(), 4u);
  EXPECT_EQ(capture.primitives.back().first, "arm.init");
}

TEST_F(OwnerInteractionTest, TrashInterruptsOwnedApproachAndResumesExactAbsoluteTarget) {
  std::atomic<std::uint64_t> owner_sequence{0};
  std::atomic<std::uint64_t> head_sequence{0};
  std::atomic<int> navigation_calls{0};
  std::atomic<bool> speech_started{false};
  std::atomic<bool> navigation_terminal{false};
  std::mutex mutex;
  std::condition_variable changed;
  bool release_navigation = false;
  std::string initial_command_id;
  std::string cancelled_command_id;
  std::vector<Json> navigation_targets;

  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) -> Json {
    if (uri.ends_with("/detection/head/state"))
      return {{"source_sequence", ++head_sequence},
              {"detections", {{{"kind", "object"},
                                {"label", "bottle"},
                                {"confidence", 0.40}}}}};
    if (uri.ends_with("/detection/state"))
      return OwnerDetection(++owner_sequence, "rest", "lie");
    if (uri.ends_with("/arm/state"))
      return {{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
              {"joint_valid", {true, true, true, true}}};
    return {{"telemetry_valid", true},
            {"fresh", true},
            {"pose_valid", true},
            {"localization_valid", true},
            {"frame_id", "map"},
            {"pose", {{"x_m", 4.0}, {"y_m", 5.0}, {"yaw_rad", 0.0}}}};
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string&, Json arguments,
          const std::string& command_id, Cancelled) -> Json {
    if (primitive.name == "speaker.play") {
      speech_started = true;
      changed.notify_all();
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!navigation_terminal && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      EXPECT_TRUE(navigation_terminal);
      EXPECT_EQ(navigation_calls.load(), 1);
      return {{"accepted", true}, {"result", {{"status", "succeeded"}}}};
    }
    if (primitive.name == "base.navigate") {
      const int call = ++navigation_calls;
      {
        std::lock_guard lock(mutex);
        navigation_targets.push_back(std::move(arguments));
        if (call == 1) initial_command_id = command_id;
      }
      if (call == 1) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(2), [&] { return release_navigation; });
        navigation_terminal = true;
        changed.notify_all();
        return {{"accepted", true}, {"result", {{"status", "cancelled"}}}};
      }
    }
    return {{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_navigation_command = [&](const std::string& command_id,
                                                const std::string& device_id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!speech_started && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(speech_started);
    EXPECT_EQ(device_id, "base-0");
    cancelled_command_id = command_id;
    {
      std::lock_guard lock(mutex);
      release_navigation = true;
    }
    changed.notify_all();
    return NavigationCancelStatus::kCancelled;
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterSkills(registry);

  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0",
      {{"robot_name", "robot"},
       {"base_device_id", "base-0"},
       {"speaker_device_id", "speaker-0"},
       {"arm_device_id", "arm-0"},
       {"switch_waypoint_id", "switch"}, {"switch_joint_positions_rad", SwitchPose()},
       {"speech_enabled", false}},
      "owner-trash-resume", Hooks());

  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_GE(head_sequence, 3u);
  EXPECT_FALSE(initial_command_id.empty());
  EXPECT_EQ(cancelled_command_id, initial_command_id);
  ASSERT_EQ(navigation_targets.size(), 2u);
  EXPECT_EQ(navigation_targets[0], navigation_targets[1]);
  EXPECT_EQ(navigation_targets[0],
            (Json{{"reference", "absolute"},
                  {"frame_id", "map"},
                  {"x_m", 5.0},
                   {"y_m", 5.0},
                   {"yaw_rad", 0.0}}));
}

}  // namespace
}  // namespace island::skills::native

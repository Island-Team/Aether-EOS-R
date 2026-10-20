#include "share/skills/native/pick_trash/pick_trash.h"
#include "share/skills/native/navigate_pick_resume/navigate_pick_resume.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
namespace {
using namespace eos::runtime::skills;
using namespace eos::runtime::skills::builtin;
using Json = nlohmann::json;

struct Call {
  std::string name;
  Json args;
};

Json Snapshot(const std::string& label = "bottle", double confidence = 0.9) {
  return {{"source_sequence", 1},
          {"detections", Json::array({{{"kind", "object"},
                                        {"label", label},
                                        {"confidence", confidence}}})}};
}

RuntimeCapabilities Capabilities(std::vector<Call>* calls) {
  RuntimeCapabilities capabilities;
  capabilities.observe_objects_fresh = [](const std::string& camera, auto, auto) {
    EXPECT_EQ(camera, "cam-0");
    return Snapshot();
  };
  capabilities.grasp_observe_joints = []() { return Json::array({0, 0, 1, 0, 1, 0}); };
  capabilities.grasp_wrist_camera = []() { return std::string("cam-0"); };
  capabilities.execute_primitive = [calls](const PrimitiveReference& primitive,
                                            const std::string&, Json arguments,
                                            const std::string&, auto) {
    calls->push_back({primitive.name, std::move(arguments)});
    return Json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  return capabilities;
}

Json Args() {
  return {{"robot_name", "demo-0"},
          {"arm_device_id", "arm-0"},
          {"speaker_device_id", "speaker-0"},
          {"observation_hold_ms", 0}};
}

TEST(PickTrash, HoldsObservationPoseBeforeCapturingAndAnnouncing) {
  eos::lib::ThreadTimerService timer;
  std::vector<Call> calls;
  auto capabilities = Capabilities(&calls);
  std::chrono::steady_clock::time_point arrived;
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive,
                                        const std::string&, Json arguments,
                                        const std::string&, auto) {
    calls.push_back({primitive.name, std::move(arguments)});
    if (primitive.name == "arm.movej") arrived = std::chrono::steady_clock::now();
    return Json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.observe_objects_fresh = [&](const std::string&, auto after, auto) {
    EXPECT_EQ(calls.size(), 1u);
    EXPECT_GE(after - arrived, std::chrono::milliseconds(40));
    return Snapshot();
  };
  Registry registry(timer, std::move(capabilities));
  RegisterPickTrash(registry);
  auto args = Args();
  args["observation_hold_ms"] = 50;

  const auto result = registry.Execute("pick_trash", "1.0.0", args, "observe-hold");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[1].name, "speaker.speak");
  EXPECT_EQ(calls[2].name, "arm.init");
}

TEST(PickTrash, ObservesAnnouncesAndResetsWithoutGripperOrPlanner) {
  eos::lib::ThreadTimerService timer;
  std::vector<Call> calls;
  Registry registry(timer, Capabilities(&calls));
  RegisterPickTrash(registry);

  const auto result = registry.Execute("pick_trash", "1.0.0", Args(), "observe-trash");

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code << ": "
                                                            << result.message;
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.result.at("observed"));
  EXPECT_EQ(result.result.at("label"), "bottle");
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[0].name, "arm.movej");
  EXPECT_EQ(calls[1].name, "speaker.speak");
  EXPECT_EQ(calls[1].args.at("text"), "这是空瓶");
  EXPECT_EQ(calls[2].name, "arm.init");
}

TEST(PickTrash, AnnouncesEverySupportedBroadClass) {
  const std::pair<const char*, const char*> cases[] = {
      {"peel", "这是果皮"},
      {"empty_box", "这是空盒子"},
      {"bottle", "这是空瓶"},
      {"paper_ball", "这是纸团"},
  };
  for (const auto& [label, announcement] : cases) {
    eos::lib::ThreadTimerService timer;
    std::vector<Call> calls;
    auto capabilities = Capabilities(&calls);
    capabilities.observe_objects_fresh = [label](const std::string&, auto, auto) {
      return Snapshot(label);
    };
    Registry registry(timer, std::move(capabilities));
    RegisterPickTrash(registry);

    const auto result = registry.Execute("pick_trash", "1.0.0", Args(), label);

    ASSERT_EQ(result.status, RunResult::Status::kCompleted) << label;
    ASSERT_EQ(calls.size(), 3u) << label;
    EXPECT_EQ(calls[1].args.at("text"), announcement) << label;
  }
}

TEST(PickTrash, EmptyObservationStillAnnouncesAndResets) {
  eos::lib::ThreadTimerService timer;
  std::vector<Call> calls;
  auto capabilities = Capabilities(&calls);
  capabilities.observe_objects_fresh = [](const std::string&, auto, auto) {
    return Json{{"source_sequence", 1}, {"detections", Json::array()}};
  };
  Registry registry(timer, std::move(capabilities));
  RegisterPickTrash(registry);

  const auto result = registry.Execute("pick_trash", "1.0.0", Args(), "observe-empty");

  ASSERT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.result.at("observed"));
  EXPECT_EQ(result.result.at("label"), "");
  ASSERT_EQ(calls.size(), 3u);
  EXPECT_EQ(calls[1].args.at("text"), "没有发现垃圾");
  EXPECT_EQ(calls[2].name, "arm.init");
}

TEST(PickTrash, NavigatePickResumeAdvertisesAllFourLitterLabels) {
  eos::lib::ThreadTimerService timer;
  Registry registry(timer);
  navigate_pick_resume::RegisterSkill(registry);
  const auto* metadata = registry.Find("navigate_pick_resume", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  const auto& labels = metadata->returns_schema.at("properties").at("trash_label").at("enum");
  EXPECT_EQ(labels, Json::array({"peel", "empty_box", "bottle", "paper_ball"}));
}

}  // namespace
}  // namespace island::skills::native

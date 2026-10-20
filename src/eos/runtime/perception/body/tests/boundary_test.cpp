#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "reader.h"
#include "detection_overlay.h"
#include "src/eos/lib/clock.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/models/camera_object.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/models/microphone_vad.h"

namespace {

using eos::lib::ManualClock;
using eos::runtime::hardware_runtime::Registry;
using eos::runtime::perception::body::Reader;

class BoundaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    registry_ = std::make_unique<Registry>(
        Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo"));
    clock_.Set(eos::lib::MonotonicTime{}, eos::lib::WallTime{std::chrono::seconds(1786871732)});
    reader_ = std::make_unique<Reader>(*registry_, clock_);
  }

  Reader& reader() { return *reader_; }

 private:
  std::unique_ptr<Registry> registry_;
  ManualClock clock_;
  std::unique_ptr<Reader> reader_;
};

void ExpectNoPerceptionData(const nlohmann::json& body) {
  const std::string text = body.dump();
  EXPECT_EQ(text.find("pcm"), std::string::npos);
  EXPECT_EQ(text.find("image_b64"), std::string::npos);
  EXPECT_EQ(text.find("transcript"), std::string::npos);
  EXPECT_EQ(text.find("detections"), std::string::npos);
  EXPECT_EQ(text.find("object_held"), std::string::npos);
  EXPECT_EQ(text.find("joint_angles"), std::string::npos);
  EXPECT_EQ(text.find("yaw_rad\""), std::string::npos);
}

TEST_F(BoundaryTest, BodyBuildersContainBoundedOperationalState) {
  const std::vector<nlohmann::json> devices = {
      reader().BuildBase(),    reader().BuildGimbal(), reader().BuildManipulator(),
      reader().BuildCamera(),  reader().BuildMic(),    reader().BuildGripper(),
      reader().BuildSpeaker(),
  };
  for (std::size_t index = 0; index < devices.size(); ++index) {
    const auto& body = devices[index];
    EXPECT_TRUE(body.contains("device_class"));
    EXPECT_TRUE(body.contains("state"));
    EXPECT_TRUE(body.contains("health"));
    EXPECT_EQ(body.size(), index == 1 ? 7u : 3u);
    ExpectNoPerceptionData(body);
  }
  EXPECT_EQ(devices[4]["device_class"], "microphone");
  EXPECT_EQ(devices[0]["state"], "idle");
  EXPECT_EQ(devices[1]["state"], "idle");
  EXPECT_EQ(devices[2]["state"], "idle");
  EXPECT_EQ(devices[3]["state"], "unknown");
  EXPECT_EQ(devices[4]["state"], "listening");
  EXPECT_EQ(devices[5]["state"], "idle");
  EXPECT_EQ(devices[6]["state"], "idle");
}

TEST_F(BoundaryTest, DetectionIsSemanticAndSeparateFromWorld) {
  EXPECT_FALSE(reader().SupportsResourceTemplate("robots/{robot_name}/world/state"));
  EXPECT_TRUE(reader().SupportsResourceTemplate("robots/{robot_name}/detection/state"));
  EXPECT_TRUE(reader().SupportsResourceTemplate("robots/{robot_name}/detection/head/state"));
  EXPECT_FALSE(reader().SupportsResourceTemplate("robots/{robot_name}/unknown/state"));
  EXPECT_THROW(reader().BuildDetection(), std::runtime_error);

  const nlohmann::json listed = reader().ListResources();
  const std::string text = listed.dump();
  EXPECT_EQ(text.find("/world/state"), std::string::npos);
  EXPECT_NE(text.find("/base/status"), std::string::npos);
  EXPECT_NE(text.find("/arm/status"), std::string::npos);
  EXPECT_NE(text.find("/gripper/status"), std::string::npos);
  EXPECT_EQ(text.find("/detection/state"), std::string::npos);
  EXPECT_EQ(text.find("/world/vision/state"), std::string::npos);
  EXPECT_NE(text.find("/devices/base-0/state"), std::string::npos);
  EXPECT_THROW(reader().BuildByUri("robots/demo-0/detection/state"), std::runtime_error);
}

TEST_F(BoundaryTest, UnifiedDeviceStateRoutesEverySupportedAdmittedDevice) {
  EXPECT_TRUE(reader().SupportsResourceTemplate("robots/{robot_name}/devices/{device_id}/state"));
  const nlohmann::json listed = reader().ListResources();
  for (const std::string device_id :
       {"base-0", "gimbal-0", "arm-0", "cam-0", "mic-0", "gripper-0", "speaker-0"}) {
    const std::string uri = "robots/demo-0/devices/" + device_id + "/state";
    EXPECT_NE(std::find_if(listed.begin(), listed.end(),
                           [&](const auto& item) { return item.value("uri", "") == uri; }),
              listed.end());
    const nlohmann::json state = reader().BuildByUri(uri);
    EXPECT_TRUE(state.contains("device_class"));
    EXPECT_TRUE(state.contains("state"));
    EXPECT_TRUE(state.contains("health"));
  }
  EXPECT_THROW(reader().BuildByUri("robots/demo-0/devices/missing/state"), std::runtime_error);
  EXPECT_THROW(reader().BuildByUri("robots/wrong/devices/base-0/state"), std::runtime_error);
  EXPECT_THROW(reader().BuildByUri("robots/demo/devices/base-0/state"), std::runtime_error);
}

TEST_F(BoundaryTest, DeviceStateRejectsADeviceOwnedByAnotherRobot) {
  EXPECT_THROW(reader().BuildDeviceState("another-robot", "base-0"), std::runtime_error);
}

TEST_F(BoundaryTest, GimbalDeviceStateExposesMeasuredPositionAndVelocity) {
  const nlohmann::json state = reader().BuildByUri("robots/demo-0/devices/gimbal-0/state");
  EXPECT_TRUE(state.at("pan_rad").is_number());
  EXPECT_TRUE(state.at("tilt_rad").is_number());
  EXPECT_TRUE(state.at("pan_rate_rad_s").is_number());
  EXPECT_TRUE(state.at("tilt_rate_rad_s").is_number());
}

TEST_F(BoundaryTest, ArmStateExposesFreshJointTelemetry) {
  EXPECT_TRUE(reader().SupportsResourceTemplate("robots/{robot_name}/arm/state"));
  const nlohmann::json arm = reader().BuildByUri("robots/demo-0/arm/state");
  EXPECT_EQ(arm["type"], "arm.snapshot");
  ASSERT_TRUE(arm["joint_positions_rad"].is_array());
  ASSERT_EQ(arm["joint_positions_rad"].size(), 6u);
  ASSERT_TRUE(arm["joint_valid"].is_array());
  ASSERT_EQ(arm["joint_valid"].size(), 6u);
  EXPECT_TRUE(arm.contains("sequence"));
  EXPECT_TRUE(arm.contains("timestamp"));
}

TEST_F(BoundaryTest, NormalizedStatusResourcesAreListedAndReadable) {
  for (const std::string device_class : {"base", "arm", "gripper"}) {
    const std::string uri = "robots/demo-0/" + device_class + "/status";
    EXPECT_TRUE(
        reader().SupportsResourceTemplate("robots/{robot_name}/" + device_class + "/status"));
    const auto listed = reader().ListResources();
    EXPECT_NE(std::find_if(listed.begin(), listed.end(),
                           [&](const auto& item) { return item.value("uri", "") == uri; }),
              listed.end());
    const nlohmann::json status = reader().BuildByUri(uri);
    EXPECT_EQ(status.at("type"), device_class + ".status");
    EXPECT_EQ(status.at("schema_version"), "1.0");
    EXPECT_TRUE(status.at("health") == "ready" || status.at("health") == "degraded" ||
                status.at("health") == "unavailable");
    EXPECT_TRUE(status.at("telemetry_valid"));
  }

  const auto base = reader().BuildByUri("robots/demo-0/base/status");
  EXPECT_TRUE(base.at("pose").contains("x_m"));
  EXPECT_TRUE(base.contains("localization_valid"));
  EXPECT_FALSE(base.contains("velocity"));
  EXPECT_FALSE(base.contains("posture"));

  const auto arm = reader().BuildByUri("robots/demo-0/arm/status");
  EXPECT_EQ(arm.at("joint_positions_rad").size(), 6u);
  EXPECT_EQ(arm.at("joint_velocities_rad_s").size(), 6u);
  EXPECT_EQ(arm.at("joint_valid").size(), 6u);
  EXPECT_EQ(arm.at("joint_velocity_valid").size(), 6u);
  EXPECT_TRUE(arm.contains("status_valid"));

  const auto gripper = reader().BuildByUri("robots/demo-0/gripper/status");
  EXPECT_EQ(gripper.at("jaw_state"), "open");
}

TEST_F(BoundaryTest, SemanticProductsAreNotFabricatedBeforeModelOutput) {
  EXPECT_THROW(reader().BuildAsr(), std::runtime_error);
  const std::string listed = reader().ListResources().dump();
  EXPECT_EQ(listed.find("/world/pose/state"), std::string::npos);
  EXPECT_EQ(listed.find("/world/vad/state"), std::string::npos);
  EXPECT_EQ(listed.find("/world/asr/state"), std::string::npos);
}

TEST(BodySemanticTest, AsrContainsOnlyCompactAgentFields) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  clock.Set(eos::lib::MonotonicTime{}, eos::lib::WallTime{std::chrono::seconds(1786871732)});
  eos::runtime::models::MicrophoneVadStore store;
  eos::runtime::models::MicrophoneVadStore::Snapshot snapshot;
  snapshot.model_id = "vad";
  snapshot.source_timestamp_ns = 12;
  snapshot.response = {{"speech", true}, {"segments", nlohmann::json::array()}};
  snapshot.transcript = "Move forward.";
  snapshot.asr_model_id = "asr";
  snapshot.asr_response = {{"text", "Move forward."},
                           {"language", "en"},
                           {"confidence", 0.95},
                           {"segments", {{{"text", "Move"}, {"start_s", 0.0}}}}};
  store.Set(std::move(snapshot));
  Reader reader(registry, clock, nullptr, nullptr, &store);

  const nlohmann::json asr = reader.BuildAsr();
  EXPECT_EQ(asr["schema_version"], "1.0");
  EXPECT_EQ(asr["text"], "Move forward.");
  EXPECT_EQ(asr["language"], "en");
  EXPECT_EQ(asr["confidence"], 0.95);
  EXPECT_EQ(asr.size(), 7u);
  EXPECT_FALSE(asr.contains("segments"));
  EXPECT_FALSE(asr.contains("source_timestamp_ns"));
  EXPECT_FALSE(asr.contains("model_id"));
}

TEST(BodySemanticTest, VadIsExposedAsACompactResource) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::MicrophoneVadStore store;
  eos::runtime::models::MicrophoneVadStore::Snapshot snapshot;
  snapshot.model_id = "vad";
  snapshot.source_timestamp_ns = 42;
  snapshot.response = {{"speech", true}, {"probability", 0.87}, {"segments", {1, 2}}};
  store.Set(std::move(snapshot));
  Reader reader(registry, clock, nullptr, nullptr, &store);

  EXPECT_TRUE(reader.SupportsResourceTemplate("robots/{robot_name}/world/vad/state"));
  const auto vad = reader.BuildByUri("robots/demo-0/world/vad/state");
  EXPECT_EQ(vad.at("type"), "vad.snapshot");
  EXPECT_EQ(vad.at("schema_version"), "1.0");
  EXPECT_TRUE(vad.at("speech"));
  EXPECT_EQ(vad.at("probability"), 0.87);
  EXPECT_EQ(vad.at("source_timestamp_ns"), 42);
  EXPECT_EQ(vad.at("source_sequence"), 1u);
  EXPECT_FALSE(vad.contains("segments"));
  EXPECT_FALSE(vad.contains("model_id"));
  EXPECT_EQ(vad.dump().find("pcm"), std::string::npos);
  EXPECT_EQ(vad.dump().find("wav"), std::string::npos);
  EXPECT_EQ(vad.dump().find("utterance"), std::string::npos);
  EXPECT_NE(reader.ListResources().dump().find("/world/vad/state"), std::string::npos);
}

TEST(BodySemanticTest, FaceIdentityIsProjectedOntoMatchedPeople) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  clock.Set(eos::lib::MonotonicTime{}, eos::lib::WallTime{std::chrono::seconds(1786871732)});
  eos::runtime::models::CameraPoseStore store;
  const std::string camera_id =
      registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  std::uint64_t next_generation = 0;

  const auto SetSnapshot = [&store, &camera_id, &next_generation](
                               std::optional<nlohmann::json> face_response,
                               const std::string& behavior_event = "none",
                               nlohmann::json canonical = nlohmann::json::object(),
                               nlohmann::json track_id = 7,
                               nlohmann::json bbox = {0.0, 0.0, 100.0, 200.0}) {
    eos::runtime::models::CameraPoseStore::Snapshot snapshot;
    snapshot.model_id = "pose";
    snapshot.device_id = camera_id;
    snapshot.generation = ++next_generation;
    snapshot.source_timestamp_ns = 12;
    snapshot.image_width_px = 1280;
    snapshot.image_height_px = 720;
    nlohmann::json person{{"track_id", std::move(track_id)},
                          {"bbox", std::move(bbox)},
                          {"confidence", 0.94},
                          {"distance_m", nullptr},
                          {"posture_state", "standing"},
                          {"posture_confidence", 0.87},
                          {"pose", "stand"},
                          {"action", behavior_event == "fallen_event" ? "fallen" :
                                     behavior_event == "waving_event" ? "waving" :
                                     behavior_event == "rest_request_candidate" ? "unknown" : "none"},
                          {"behavior_event", behavior_event},
                          {"behavior_confidence", 0.12},
                          {"pose_confidence", 0.87},
                          {"action_confidence", 0.12},
                          {"keypoints", nlohmann::json::array()},
                          {"keypoint_scores", nlohmann::json::array()}};
    person.update(canonical);
    snapshot.response = {{"persons", {std::move(person)}}, {"person_count", 1}};
    snapshot.face_response = std::move(face_response);
    store.Set(std::move(snapshot));
  };
  const auto Human = [](const nlohmann::json& detection) -> const nlohmann::json& {
    return *std::find_if(detection.at("detections").begin(), detection.at("detections").end(),
                         [](const auto& item) { return item.at("kind") == "human"; });
  };

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data",
                 {{"faces",
                   {{{"identity", "Alice"},
                     {"known", true},
                     {"similarity", 0.97},
                     {"bbox", {10.0, 10.0, 30.0, 40.0}}}}},
                  {"face_count", 1}}}}});
  Reader reader(registry, clock, &store);
  const nlohmann::json recognized = reader.BuildDetection();
  EXPECT_EQ(recognized.at("image_width_px"), 1280);
  EXPECT_EQ(recognized.at("image_height_px"), 720);
  EXPECT_EQ(Human(recognized).at("bbox"), nlohmann::json({0.0, 0.0, 100.0, 200.0}));
  EXPECT_EQ(Human(recognized).at("attributes").at("name"), "Alice");
  EXPECT_EQ(Human(recognized).at("attributes").at("name_source_generation"), 1);
  EXPECT_DOUBLE_EQ(Human(recognized).at("attributes").at("name_similarity"), 0.97);
  EXPECT_EQ(Human(recognized).at("attributes").at("pose"), "stand");
  EXPECT_EQ(Human(recognized).at("attributes").at("pose_confidence"), 0.87);
  EXPECT_FALSE(Human(recognized).at("attributes").contains("posture_state"));
  EXPECT_EQ(Human(recognized).at("attributes").at("action"), "unknown");
  EXPECT_FALSE(Human(recognized).at("attributes").contains("action_confidence"));
  EXPECT_EQ(Human(recognized).at("attributes").at("activity"), "stand");

  SetSnapshot(std::nullopt, "none", {{"posture_source", "gemma4"},
                                     {"action_source", "gemma4"}, {"posture_age_ms", 1200}});
  const auto enriched = reader.BuildDetection();
  EXPECT_EQ(Human(enriched).at("attributes").at("posture_source"), "gemma4");
  EXPECT_EQ(Human(enriched).at("attributes").at("action_source"), "gemma4");
  EXPECT_EQ(Human(enriched).at("attributes").at("posture_age_ms"), 1200);
  EXPECT_EQ(Human(enriched).at("attributes").at("activity"), "stand");

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data",
                 {{"faces",
                   {{{"identity", "nearest"},
                     {"known", false},
                     {"similarity", 0.2},
                     {"bbox", {10.0, 10.0, 30.0, 40.0}}}}},
                  {"face_count", 1}}}}});
  const auto retained_unknown = Human(reader.BuildDetection()).at("attributes");
  EXPECT_EQ(retained_unknown.at("name"), "Alice");
  EXPECT_EQ(retained_unknown.at("name_source_generation"), 1);
  EXPECT_DOUBLE_EQ(retained_unknown.at("name_similarity"), 0.97);

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data", {{"faces", nlohmann::json::array()}, {"face_count", 0}}}}});
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("name"), "Alice");

  SetSnapshot(std::nullopt);
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("name"), "Alice");

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data",
                 {{"faces",
                   {{{"identity", "nearest"},
                     {"known", true},
                     {"similarity", 0.39},
                     {"bbox", {10.0, 10.0, 30.0, 40.0}}}}},
                  {"face_count", 1}}}}});
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("name"), "Alice");

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data",
                 {{"faces",
                   {{{"identity", "Bob"},
                     {"known", true},
                     {"similarity", 0.40},
                     {"bbox", {10.0, 10.0, 30.0, 40.0}}}}},
                  {"face_count", 1}}}}});
  const auto bob = Human(reader.BuildDetection()).at("attributes");
  EXPECT_EQ(bob.at("name"), "Bob");
  EXPECT_EQ(bob.at("name_source_generation"), next_generation);
  EXPECT_DOUBLE_EQ(bob.at("name_similarity"), 0.40);

  SetSnapshot(std::nullopt, "none", nlohmann::json::object(), 8);
  EXPECT_TRUE(Human(reader.BuildDetection()).at("attributes").at("name").is_null());

  SetSnapshot({{{"status", "success"},
                {"operation", "recognize"},
                {"data",
                 {{"faces",
                   {{{"identity", "nearest"},
                     {"known", false},
                     {"similarity", 0.2},
                     {"bbox", {10.0, 10.0, 30.0, 40.0}}}}},
                  {"face_count", 1}}}}},
              "none", nlohmann::json::object(), 8);
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("name"), "unknown");

  SetSnapshot(std::nullopt, "rest_request_candidate");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("action"), "unknown");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("activity"), "stand");
  SetSnapshot(std::nullopt, "fallen_event");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("action"), "unknown");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("activity"), "stand");
  SetSnapshot(std::nullopt, "waving_event");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("action"), "waving");
  EXPECT_EQ(Human(reader.BuildDetection()).at("attributes").at("activity"), "waving");

  SetSnapshot(std::nullopt, "fallen_event", {{"pose", "sit"}, {"action", "none"}});
  const nlohmann::json canonical = Human(reader.BuildDetection()).at("attributes");
  EXPECT_EQ(canonical.at("pose"), "sit");
  EXPECT_EQ(canonical.at("action"), "unknown");
  EXPECT_EQ(canonical.at("activity"), "sit");

  SetSnapshot(std::nullopt, "none", {{"pose", "lie"}, {"action", "rest"}});
  const nlohmann::json resting = Human(reader.BuildDetection()).at("attributes");
  EXPECT_EQ(resting.at("pose"), "lie");
  EXPECT_EQ(resting.at("action"), "unknown");
  EXPECT_EQ(resting.at("activity"), "lie");
  EXPECT_FALSE(resting.contains("posture_state"));
}

TEST(BodySemanticTest, PublishesOnlyHumanBoxesWithAtLeastPointFourConfidence) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::CameraPoseStore store;
  eos::runtime::models::CameraPoseStore::Snapshot source;
  source.model_id = "human";
  source.device_id = registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  source.generation = 1;
  source.image_width_px = 640;
  source.image_height_px = 480;
  store.SetSource(std::move(source), true, false, false, false);
  ASSERT_TRUE(store.SetHuman(
      1, {{"persons", {{{"track_id", 1}, {"confidence", 0.39}},
                       {{"track_id", 2}, {"confidence", 0.4}},
                       {{"track_id", 3}, {"confidence", 0.9}}}},
          {"person_count", 3}}));

  Reader reader(registry, clock, &store);
  const auto detections = reader.BuildDetection().at("detections");
  ASSERT_EQ(detections.size(), 2u);
  EXPECT_EQ(detections.at(0).at("id"), "human:2");
  EXPECT_EQ(detections.at(1).at("id"), "human:3");
}

TEST(BodySemanticTest, ProjectsIndependentAsynchronousModelState) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::CameraPoseStore store;
  const std::string camera_id =
      registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  eos::runtime::models::CameraPoseStore::Snapshot source;
  source.model_id = "human";
  source.device_id = camera_id;
  source.generation = 21;
  source.image_width_px = 1920;
  source.image_height_px = 1080;
  store.SetSource(std::move(source), true, true, true, true);

  Reader reader(registry, clock, &store);
  const auto pending = reader.BuildDetection();
  EXPECT_EQ(pending.at("source_sequence"), 1u);
  EXPECT_EQ(pending.at("source_generation"), 21u);
  EXPECT_TRUE(pending.at("detections").empty());
  EXPECT_EQ(pending.at("models").at("human").at("status"), "pending");
  EXPECT_EQ(pending.at("models").at("pose").at("status"), "pending");
  EXPECT_EQ(pending.at("models").at("action").at("status"), "pending");
  EXPECT_EQ(pending.at("models").at("face").at("status"), "pending");
  EXPECT_EQ(pending.at("face_recognition_status"), "pending");

  ASSERT_TRUE(store.SetHuman(
      21, {{"persons", {{{"track_id", 1}, {"confidence", 0.9}}}},
           {"person_count", 1},
           {"furniture", {{{"bbox", {10.0, 20.0, 110.0, 220.0}}, {"class_name", "chair"}}}}}));
  const auto completed = reader.BuildDetection();
  EXPECT_EQ(completed.at("source_sequence"), 1u);
  EXPECT_EQ(completed.at("models").at("human").at("status"), "complete");
  EXPECT_EQ(completed.at("models").at("human").at("sequence"), 1u);
  EXPECT_EQ(completed.at("models").at("human").at("source_generation"), 21u);
  EXPECT_EQ(completed.at("models").at("human").at("requested_generation"), 21u);
  EXPECT_EQ(completed.at("models").at("pose").at("status"), "pending");
  ASSERT_EQ(completed.at("detections").size(), 2u);
  const auto& furniture = completed.at("detections").at(1);
  EXPECT_EQ(furniture.at("id"), "obj:furniture-chair-0");
  EXPECT_EQ(furniture.at("kind"), "object");
  EXPECT_EQ(furniture.at("label"), "chair");
  EXPECT_FALSE(furniture.contains("confidence"));
  EXPECT_EQ(furniture.at("bbox"), nlohmann::json({10.0, 20.0, 110.0, 220.0}));
  EXPECT_TRUE(furniture.at("attributes").at("furniture"));
}

TEST(BodySemanticTest, ProjectsExecutionOverlayOnlyForMatchingCameraAndLiveHuman) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::CameraPoseStore store;
  eos::runtime::perception::body::DetectionOverlayStore overlays;
  const std::string camera_id =
      registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  eos::runtime::models::CameraPoseStore::Snapshot source;
  source.model_id = "human";
  source.device_id = camera_id;
  source.generation = 5;
  store.SetSource(std::move(source), true, false, false, false);
  Reader reader(registry, clock, &store, nullptr, nullptr, &overlays);
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  nlohmann::json depths = nlohmann::json::array();
  for (int i = 0; i < 17; ++i) {
    points.push_back({i + 10, i + 20});
    scores.push_back(0.9);
    depths.push_back(1.5);
  }
  overlays.Upsert("execution-1",
                  {{"source", camera_id},
                   {"source_generation", 5u},
                   {"target_bbox", {10, 20, 50, 80}},
                   {"detection", {{"id", "human:rotated-1"},
                                  {"kind", "human"},
                                  {"label", "person"},
                                  {"bbox", {10, 20, 50, 80}},
                                  {"keypoints", points},
                                  {"keypoint_scores", scores},
                                  {"keypoint_depth_m", depths},
                                  {"attributes", {{"pose", "lie"},
                                                  {"action", "fallen"},
                                                  {"activity", "fallen"}}}}}});
  const auto confirmed = reader.BuildDetection();
  ASSERT_EQ(confirmed["detections"].size(), 1u);
  EXPECT_EQ(confirmed["detections"][0]["id"], "human:rotated-1");
  EXPECT_EQ(confirmed["detections"][0]["bbox"], nlohmann::json::array({10, 20, 50, 80}));
  EXPECT_EQ(confirmed["detections"][0]["keypoints"], points);
  EXPECT_EQ(confirmed["detections"][0]["keypoint_depth_m"], depths);

  eos::runtime::models::CameraPoseStore::Snapshot later;
  later.model_id = "human";
  later.device_id = camera_id;
  later.generation = 6;
  store.SetSource(std::move(later), true, false, false, false);
  ASSERT_TRUE(store.SetHuman(6, {{"persons", {{{"track_id", "new-track"},
                                               {"confidence", 0.91},
                                               {"bbox", {12, 22, 52, 82}},
                                               {"pose", "stand"}}}},
                                 {"person_count", 1}}));
  const auto matched = reader.BuildDetection();
  ASSERT_EQ(matched["detections"].size(), 1u);
  EXPECT_EQ(matched["detections"][0]["id"], "human:new-track");
  EXPECT_EQ(matched["detections"][0]["bbox"], nlohmann::json::array({12, 22, 52, 82}));
  EXPECT_EQ(matched["detections"][0]["confidence"], 0.91);
  EXPECT_FALSE(matched["detections"][0].contains("keypoint_depth_m"));
  EXPECT_EQ(matched["detections"][0]["attributes"]["pose"], "lie");
  overlays.Clear("execution-1");
  EXPECT_EQ(reader.BuildDetection()["detections"][0]["attributes"]["pose"], "stand");
}

TEST(BodySemanticTest, ProjectsCaptureGimbalAndMatchedPoseJointsWithProvenance) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::CameraPoseStore store;
  const std::string camera_id =
      registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  eos::runtime::models::CameraPoseStore::Snapshot source;
  source.model_id = "human";
  source.device_id = camera_id;
  source.generation = 31;
  source.source_timestamp_ns = 9876;
  source.source_clock = eos::hardware::SourceClockKind::kRos;
  source.source_clock_id = "camera-clock";
  source.capture_gimbal = eos::hardware::GimbalTelemetry{0.3, -0.2, 0.04, -0.01};
  store.SetSource(std::move(source), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      31, {{"persons", {{{"track_id", 7}, {"confidence", 0.9}}}}, {"person_count", 1}}));
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  nlohmann::json depths = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    points.push_back({index + 0.1, index + 0.2});
    scores.push_back(0.8);
    depths.push_back(index == 3 ? nlohmann::json(nullptr) : nlohmann::json(1.5));
  }
  ASSERT_TRUE(store.MergePose(31,
                              {{"persons",
                                {{{"track_id", 7},
                                  {"keypoints", points},
                                  {"keypoint_scores", scores},
                                  {"keypoint_depth_m", depths},
                                  {"pose", "stand"}}}},
                               {"person_count", 1}},
                              9876, eos::hardware::SourceClockKind::kRos, "camera-clock"));

  Reader reader(registry, clock, &store);
  const auto detection = reader.BuildDetection();
  EXPECT_EQ(detection.at("source_gimbal").at("pan_rad"), 0.3);
  EXPECT_EQ(detection.at("pose_observation").at("source_generation"), 31);
  EXPECT_EQ(detection.at("pose_observation").at("source_timestamp_ns"), 9876);
  EXPECT_EQ(detection.at("pose_observation").at("source_clock"), "ros");
  ASSERT_EQ(detection.at("detections").size(), 1u);
  EXPECT_EQ(detection.at("detections").at(0).at("keypoints"), points);
  EXPECT_EQ(detection.at("detections").at(0).at("keypoint_scores"), scores);
  EXPECT_EQ(detection.at("detections").at(0).at("keypoint_depth_m"), depths);
}

TEST(BodySemanticTest, AsyncFaceIdentityFollowsExplicitTrackAcrossBboxMotion) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  clock.Set(eos::lib::MonotonicTime{}, eos::lib::WallTime{std::chrono::seconds(1786871732)});
  eos::runtime::models::CameraPoseStore store;
  const std::string camera_id =
      registry.Find(eos::hardware::DeviceKind::kCamera).identity().device_id;
  auto source = eos::runtime::models::CameraPoseStore::Snapshot{};
  source.model_id = "pose";
  source.device_id = camera_id;
  source.generation = 10;
  source.face_requested = true;
  source.response = {
      {"persons", {{{"track_id", 7}, {"bbox", {0.0, 0.0, 100.0, 200.0}}, {"confidence", 0.94}}}}};
  store.SetPose(std::move(source), true);

  auto latest = eos::runtime::models::CameraPoseStore::Snapshot{};
  latest.model_id = "pose";
  latest.device_id = camera_id;
  latest.generation = 11;
  latest.face_requested = true;
  latest.response = {
      {"persons", {{{"track_id", 7}, {"bbox", {10.0, 0.0, 110.0, 200.0}}, {"confidence", 0.95}}}}};
  store.SetPose(std::move(latest), true);
  const nlohmann::json face_response = {{"data",
                                         {{"faces",
                                           {{{"identity", "Alice"},
                                             {"known", true},
                                             {"similarity", 0.97},
                                             {"bbox", {10.0, 10.0, 30.0, 40.0}}}}}}}};
  store.SetFace(10, face_response);

  Reader reader(registry, clock, &store);
  const nlohmann::json detection = reader.BuildDetection();
  EXPECT_EQ(detection.at("source_generation"), 11);
  EXPECT_EQ(detection.at("source_sequence"), 2);
  EXPECT_EQ(detection.at("face_observation").at("source_generation"), 10);
  EXPECT_GE(detection.at("face_observation").at("age_ms"), 0);
  EXPECT_EQ(detection.at("detections").at(0).at("attributes").at("name"), "Alice");
  EXPECT_EQ(detection.at("detections").at(0).at("attributes").at("name_source_generation"), 10);
  EXPECT_DOUBLE_EQ(detection.at("detections").at(0).at("attributes").at("name_similarity"),
                   0.97);

  auto jumped = eos::runtime::models::CameraPoseStore::Snapshot{};
  jumped.model_id = "pose";
  jumped.device_id = camera_id;
  jumped.generation = 12;
  jumped.face_requested = true;
  jumped.response = {
      {"persons", {{{"track_id", 7}, {"bbox", {300.0, 0.0, 400.0, 200.0}}, {"confidence", 0.95}}}}};
  store.SetPose(std::move(jumped), true);
  const auto retained = reader.BuildDetection().at("detections").at(0).at("attributes");
  EXPECT_EQ(retained.at("name"), "Alice");
  EXPECT_EQ(retained.at("name_source_generation"), 10);
}

TEST(BodySemanticTest, PublishesDefaultCameraObjectsOnIndependentResource) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  clock.Set(eos::lib::MonotonicTime{}, eos::lib::WallTime{std::chrono::seconds(1786871732)});
  eos::runtime::models::CameraObjectStore store;
  eos::runtime::models::CameraObjectStore::Snapshot snapshot;
  snapshot.model_id = "object";
  snapshot.device_id = "cam-0";
  snapshot.part_id = "default";
  snapshot.image_width_px = 640;
  snapshot.image_height_px = 480;
  snapshot.response = nlohmann::json{{"objects",
                                       {{{"label", "paper cup"},
                                         {"class_id", 41},
                                         {"source_label", "paper"},
                                         {"source_class_id", 5},
                                        {"confidence", 0.88},
                                        {"bbox", {100.0, 120.0, 180.0, 240.0}},
                                        {"polygon", {{100.0, 120.0}, {180.0, 120.0},
                                                     {180.0, 240.0}}}}}},
                                     {"object_count", 1}};
  store.Set(std::move(snapshot));

  Reader reader(registry, clock, nullptr, &store, nullptr, nullptr, nullptr, "cam-0");
  const auto detection = reader.BuildByUri("robots/demo-0/cameras/cam-0/detections/state");
  ASSERT_EQ(detection.at("detections").size(), 1u);
  EXPECT_EQ(detection.at("detections").at(0).at("id"), "obj:paper-cup-0");
  EXPECT_EQ(detection.at("detections").at(0).at("kind"), "object");
  EXPECT_EQ(detection.at("detections").at(0).at("class_id"), 41);
  EXPECT_EQ(detection.at("detections").at(0).at("source_label"), "paper");
  EXPECT_EQ(detection.at("detections").at(0).at("source_class_id"), 5);
  EXPECT_EQ(detection.at("detections").at(0).at("bbox"),
            nlohmann::json({100.0, 120.0, 180.0, 240.0}));
  EXPECT_EQ(detection.at("detections").at(0).at("polygon"),
            nlohmann::json::array({{100.0, 120.0}, {180.0, 120.0}, {180.0, 240.0}}));

  eos::runtime::models::CameraObjectStore::Snapshot miss;
  miss.model_id = "object";
  miss.device_id = "cam-0";
  miss.part_id = "default";
  miss.response = nlohmann::json{{"objects", nlohmann::json::array()}, {"object_count", 0}};
  store.Set(std::move(miss));
  EXPECT_TRUE(reader.BuildByUri("robots/demo-0/cameras/cam-0/detections/state").at("detections").empty());
  EXPECT_EQ(reader.BuildByUri("robots/demo-0/detection/head/state").at("source"), "cam-0");
}

TEST(BodySemanticTest, DetectionRejectsSnapshotWithoutBoundCameraSource) {
  Registry registry =
      Registry::Load(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo", "demo");
  ManualClock clock;
  eos::runtime::models::CameraPoseStore store;
  eos::runtime::models::CameraPoseStore::Snapshot snapshot;
  snapshot.model_id = "pose";
  snapshot.device_id = "claimed-camera";
  snapshot.response = {{"persons", nlohmann::json::array()}, {"person_count", 0}};
  store.Set(std::move(snapshot));
  Reader reader(registry, clock, &store);

  EXPECT_THROW(reader.BuildDetection(), std::runtime_error);
}

}  // namespace

/// SchemaStore unit tests against the repo's real share/contracts/ tree:
/// load and index counts, the docs' wire examples (docs/eos/contracts.md)
/// validating, negative cases with rich error details, and load failures.

#include "src/eos/runtime/init/schemas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <vector>

namespace eos::runtime::init {
namespace {

/// Parses a raw JSON literal the way a frame off the wire would arrive.
nlohmann::json Wire(const std::string& text) { return nlohmann::json::parse(text); }

/// Runs `validate` and returns the thrown ValidationError's issues; fails the
/// test when the instance was accepted.
std::vector<SchemaIssue> ExpectRejected(const std::function<void()>& validate) {
  try {
    validate();
  } catch (const ValidationError& e) {
    return e.Issues();
  }
  ADD_FAILURE() << "expected ValidationError";
  return {};
}

/// Set-equality helper: `names` contains exactly the expected strings.
void ExpectNames(std::vector<std::string> names, const std::vector<std::string>& expected) {
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, expected);
}

class SchemaStoreTest : public ::testing::Test {
 protected:
  SchemaStore store_{std::filesystem::path(EOS_CONTRACTS_ROOT)};
};

TEST_F(SchemaStoreTest, LoadsAllCanonicalSchemas) { EXPECT_GE(store_.Size(), 32u); }

TEST_F(SchemaStoreTest, PrimitiveMapHoldsFifteenPrimitives) {
  const auto identities = store_.PrimitiveIdentities();
  ASSERT_EQ(identities.size(), 15u);
  std::vector<std::string> names;
  for (const auto& identity : identities) names.push_back(identity.name);
  ExpectNames(std::move(names),
              {"arm.init", "arm.movej", "arm.movel", "arm.pick", "base.navigate", "base.posture",
               "base.velocity", "face.register", "gimbal.motion", "gripper.motion", "motion.stop",
               "robot.stop", "safety.estop", "speaker.play", "speaker.speak"});
  EXPECT_TRUE(store_.HasPrimitive("arm.movej", "1.0.0"));
  EXPECT_TRUE(store_.HasPrimitive("speaker.play", "1.0.0"));
  EXPECT_TRUE(store_.HasPrimitive("gripper.motion", "1.0.0"));
}

TEST_F(SchemaStoreTest, TestsPrimitiveMembershipWithoutMaterializingTheRegistry) {
  EXPECT_TRUE(store_.HasPrimitive("base.velocity", "1.0.0"));
  EXPECT_FALSE(store_.HasPrimitive("base.velocity", "2.0.0"));
  EXPECT_FALSE(store_.HasPrimitive("arm.wave", "1.0.0"));
  EXPECT_TRUE(store_.HasPrimitive("arm.pick", "1.0.0"));
}

TEST_F(SchemaStoreTest, ReturnsRawPrimitiveSchemasByExactIdentity) {
  const nlohmann::json schema = store_.PrimitiveSchema("arm.movej", "1.0.0");
  EXPECT_EQ(schema.at("properties").at("type").at("const"), "arm.movej");
  EXPECT_EQ(schema.at("properties").at("joint_positions_rad").at("maxItems"), 6);
  EXPECT_THROW(store_.PrimitiveSchema("arm.movej", "2.0.0"), ValidationError);
}

TEST_F(SchemaStoreTest, ResourceMapContainsOnlyWorldProducts) {
  ExpectNames(store_.ResourceTypes(),
              {"arm.snapshot", "arm.status", "asr.snapshot", "base.status", "detection.snapshot",
               "gripper.status", "vad.snapshot", "vision.snapshot", "world.snapshot"});
}

TEST_F(SchemaStoreTest, BodyDeviceSchemasValidateByIdAndRejectPerceptionFields) {
  struct BodyCase {
    const char* path;
    nlohmann::json value;
    const char* forbidden_field;
  };
  const std::vector<BodyCase> cases = {
      {"body/base.v1.json",
       {{"device_class", "base"},
        {"configuration",
         {{"footprint_length_m", 0.5},
          {"footprint_width_m", 0.3},
          {"supported_modes", {"differential"}}}},
        {"operational_state", {{"controller_state", "idle"}, {"motors_enabled", true}}}},
       "pose"},
      {"body/manipulator.v1.json",
       {{"device_class", "manipulator"},
        {"configuration",
         {{"joint_names", {"shoulder", "elbow"}},
          {"joint_count", 2},
          {"end_effector_type", "gripper"},
          {"mount_frame", "base"}}},
        {"operational_state", {{"controller_state", "idle"}, {"calibrated", true}}}},
       "joint_angles_rad"},
      {"body/gimbal.v1.json",
       {{"device_class", "gimbal"},
        {"configuration",
         {{"pan_min_rad", -1.0},
          {"pan_max_rad", 1.0},
          {"tilt_min_rad", -0.5},
          {"tilt_max_rad", 0.5},
          {"fov_rad", 1.2},
          {"range_m", nullptr}}},
        {"operational_state", {{"enabled", true}, {"controller_state", "idle"}}}},
       "pan_rad"},
      {"body/gripper.v1.json",
       {{"device_class", "gripper"},
        {"configuration",
         {{"supported_actions", {"open", "close"}},
          {"supports_force_control", true},
          {"supports_position_control", false}}},
        {"operational_state", {{"jaw_state", "open"}}}},
       "measured_force_n"},
      {"body/camera.v1.json",
       {{"device_class", "camera"},
        {"configuration",
         {{"width_px", 640},
          {"height_px", 480},
          {"frame_rate_hz", 30.0},
          {"pixel_format", "MJPEG"},
          {"fov_rad", 1.1}}},
        {"operational_state", {{"present", true}, {"capturing", true}}}},
       "image_b64"},
      {"body/mic.v1.json",
       {{"device_class", "microphone"},
        {"configuration",
         {{"sample_rate_hz", 16000},
          {"channels", 1},
          {"sample_format", "s16le"},
          {"window_duration_ms", 500}}},
        {"operational_state", {{"present", true}, {"capturing", true}}}},
       "pcm_b64"},
      {"body/speaker.v1.json",
       {{"device_class", "speaker"},
        {"configuration",
         {{"supported_media_types", {"audio/wav"}}, {"sample_rates_hz", {16000}}, {"channels", 1}}},
        {"operational_state",
         {{"present", true}, {"playback_state", "idle"}, {"buffered_duration_ms", 0}}}},
       "audio_b64"},
  };

  for (const BodyCase& body : cases) {
    EXPECT_NO_THROW(store_.ValidateById(body.path, body.value)) << body.path;
    nlohmann::json polluted = body.value;
    polluted[body.forbidden_field] = "forbidden";
    EXPECT_THROW(store_.ValidateById(body.path, polluted), ValidationError) << body.path;
  }

  nlohmann::json microphone = cases[5].value;
  microphone["asr"] = {{"text", "not body state"}};
  EXPECT_THROW(store_.ValidateById(cases[5].path, microphone), ValidationError);
  microphone = cases[5].value;
  microphone["vad"] = {{"speech", true}};
  EXPECT_THROW(store_.ValidateById(cases[5].path, microphone), ValidationError);
}

TEST_F(SchemaStoreTest, ValidatesIndependentWorldProducts) {
  store_.ValidateResource("world.snapshot", Wire(R"json({
    "type":"world.snapshot","schema_version":"1.0","sequence":1,
    "timestamp":"2026-09-21T00:00:00Z","frame_id":"map",
    "entities":[{"id":"obj:cup-1","label":"cup","position_m":[1.0,2.0,0.8]}],
    "relations":[]
  })json"));
  store_.ValidateResource("vision.snapshot", Wire(R"json({
    "type":"vision.snapshot","schema_version":"1.0","deviceId":"cam-0",
    "frames":[{"jpeg":"/9j/","widthPx":640,"heightPx":480,
    "capturedAt":"2026-09-21T00:00:00Z"}]
  })json"));
  store_.ValidateResource("detection.snapshot", Wire(R"json({
    "type":"detection.snapshot","schema_version":"1.0","sequence":4,
    "timestamp":"2026-09-21T00:00:00Z","source":"cam-0",
    "detections":[{"id":"human:7","kind":"human","label":"person","confidence":0.95,
    "attributes":{"name":"Alice","pose":"stand","action":"waving","activity":"waving"}}]
  })json"));
  store_.ValidateResource("detection.snapshot", Wire(R"json({
    "type":"detection.snapshot","schema_version":"1.0","sequence":5,
    "timestamp":"2026-09-21T00:00:00Z","source":"cam-0",
    "detections":[{"id":"human:8","kind":"human","label":"person","confidence":0.90,
    "attributes":{"name":null}}]
  })json"));
  nlohmann::json invalid_name = Wire(R"json({
    "type":"detection.snapshot","schema_version":"1.0","sequence":6,
    "timestamp":"2026-09-21T00:00:00Z","source":"cam-0",
    "detections":[{"id":"human:9","kind":"human","label":"person","confidence":0.90,
    "attributes":{"name":true}}]
  })json");
  EXPECT_THROW(store_.ValidateResource("detection.snapshot", invalid_name), ValidationError);
  invalid_name["detections"][0]["attributes"] = {{"pose", "standing"}};
  EXPECT_THROW(store_.ValidateResource("detection.snapshot", invalid_name), ValidationError);
  store_.ValidateResource("asr.snapshot", Wire(R"json({
    "type":"asr.snapshot","schema_version":"1.0","sequence":5,
    "timestamp":"2026-09-21T00:00:00Z","text":"hello","language":"en","confidence":0.9
  })json"));
  store_.ValidateResource("vad.snapshot", Wire(R"json({
    "type":"vad.snapshot","schema_version":"1.0","sequence":8,
    "timestamp":"2026-09-21T00:00:00Z","source_sequence":17,
    "source_timestamp_ns":1789983000000000000,"speech":true,"probability":0.91
  })json"));
}

TEST_F(SchemaStoreTest, SemanticProductsRejectHeavyOrRawPerceptionData) {
  nlohmann::json vad = Wire(R"json({
    "type":"vad.snapshot","schema_version":"1.0","sequence":1,
    "timestamp":"2026-09-21T00:00:00Z","source_sequence":1,"speech":false,"probability":0.0
  })json");
  EXPECT_NO_THROW(store_.ValidateResource("vad.snapshot", vad));
  for (const char* field : {"pcm_b64", "wav", "utterance", "segments", "model_id"}) {
    nlohmann::json polluted = vad;
    polluted[field] = field;
    EXPECT_THROW(store_.ValidateResource("vad.snapshot", polluted), ValidationError) << field;
  }
  vad["probability"] = 1.01;
  EXPECT_THROW(store_.ValidateResource("vad.snapshot", vad), ValidationError);
  nlohmann::json asr = Wire(R"json({
    "type":"asr.snapshot","schema_version":"1.0","sequence":1,
    "timestamp":"2026-09-21T00:00:00Z","text":"hello"
  })json");
  EXPECT_NO_THROW(store_.ValidateResource("asr.snapshot", asr));
  asr["vad"] = true;
  EXPECT_THROW(store_.ValidateResource("asr.snapshot", asr), ValidationError);
  nlohmann::json detection = Wire(R"json({
    "type":"detection.snapshot","schema_version":"1.0","sequence":1,
    "timestamp":"2026-09-21T00:00:00Z","source":"cam-0",
    "detections":[{"id":"human:7","kind":"human","label":"person","confidence":0.9}]
  })json");
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  nlohmann::json depths = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    points.push_back({index + 0.25, index + 0.5});
    scores.push_back(0.9);
    depths.push_back(index == 0 ? nlohmann::json(nullptr) : nlohmann::json(1.25));
  }
  detection["detections"][0]["keypoints"] = points;
  detection["detections"][0]["keypoint_scores"] = scores;
  detection["detections"][0]["keypoint_depth_m"] = depths;
  detection["source_gimbal"] = {
      {"pan_rad", 0.1}, {"tilt_rad", -0.2}, {"pan_rate_rad_s", 0.01}, {"tilt_rate_rad_s", -0.02}};
  detection["pose_observation"] = {{"source_generation", 1},
                                   {"source_timestamp_ns", 123},
                                   {"source_clock", "ros"},
                                   {"source_clock_id", "camera"},
                                   {"age_ms", 25}};
  EXPECT_NO_THROW(store_.ValidateResource("detection.snapshot", detection));
  auto& invalid_points = detection["detections"][0]["keypoints"];
  invalid_points.erase(std::prev(invalid_points.end()));
  EXPECT_THROW(store_.ValidateResource("detection.snapshot", detection), ValidationError);
}

TEST_F(SchemaStoreTest, AcceptsAckAcceptedAndRejected) {
  store_.ValidateAck(Wire(R"json({
    "type": "primitive.ack", "primitive_version": "1.0.0", "primitive": "arm.movel", "accepted": true,
    "reason": null, "reason_code": null, "command_id": "cmd-1041", "sequence": 413,
    "result": {"status": "succeeded"}
  })json"));
  store_.ValidateAck(Wire(R"json({
    "type": "primitive.ack", "primitive_version": "1.0.0", "primitive": "arm.movel", "accepted": false,
    "reason": "object is not pickable", "reason_code": "rejected.invalid_state",
    "command_id": "cmd-1041", "sequence": 413, "result": {"status": "rejected"}
  })json"));
}

TEST_F(SchemaStoreTest, RejectsContradictoryAcknowledgementsAndVersions) {
  EXPECT_THROW(store_.ValidateAck(Wire(R"json({
    "type": "primitive.ack", "primitive_version": "1.0.0", "primitive": "arm.movel",
    "accepted": true, "reason": "stopped", "reason_code": "rejected.estop", "sequence": 1
  })json")),
               ValidationError);
  EXPECT_THROW(store_.ValidateAck(Wire(R"json({
    "type": "primitive.ack", "primitive_version": "1.0.0", "primitive": "arm.movel",
    "accepted": false, "reason": null, "reason_code": null, "sequence": 2
  })json")),
               ValidationError);
  EXPECT_THROW(store_.ValidateAck(Wire(R"json({
    "type": "primitive.ack", "primitive_version": "1.0", "primitive": "arm.movel",
    "accepted": true, "reason": null, "reason_code": null, "sequence": 3
  })json")),
               ValidationError);
  EXPECT_THROW(
      store_.ValidatePrimitive("base.posture", "2.0.0",
                               Wire(R"json({"type":"base.posture","posture":"stand"})json")),
      ValidationError);
  EXPECT_THROW(
      store_.ValidatePrimitive(
          "base.posture", "1.0.0",
          Wire(R"json({"type":"base.posture","schema_version":"1.0","posture":"stand"})json")),
      ValidationError);
}

TEST_F(SchemaStoreTest, AcceptsPrimitiveRequests) {
  store_.ValidatePrimitive("arm.init", "1.0.0", Wire(R"json({"type": "arm.init"})json"));
  store_.ValidatePrimitive("arm.movel", "1.0.0", Wire(R"json({"type": "arm.movel",
                             "command_id": "cmd-1041", "position_m": [0.3, 0.0, 0.2],
                             "orientation_quat_xyzw": [0.0, 0.0, 0.0, 1.0],
                             "reference": "relative"})json"));
  store_.ValidatePrimitive("base.velocity", "1.0.0", Wire(R"json({"type": "base.velocity",
                                               "command_id": "cmd-1042", "linear_x_m_s": 0.3,
                                               "linear_y_m_s": 0.0, "angular_z_rad_s": 0.1,
                                               "valid_for_ms": 250})json"));
  store_.ValidatePrimitive("base.navigate", "1.0.0", Wire(R"json({"type": "base.navigate",
                    "command_id": "cmd-1043", "x_m": 1.0, "y_m": -0.5,
                    "yaw_rad": 0.25, "reference": "relative"})json"));
  store_.ValidatePrimitive("base.navigate", "1.0.0", Wire(R"json({"type":"base.navigate",
                    "reference":"absolute","frame_id":"map","x_m":14.525529,
                    "y_m":10.965195,"yaw_rad":-3.0782821,"z_m":-0.53097105,
                    "speed":1})json"));
  EXPECT_THROW(store_.ValidatePrimitive("base.navigate", "1.0.0",
               Wire(R"json({"type":"base.navigate","reference":"absolute","frame_id":"map",
                     "x_m":1,"y_m":2,"yaw_rad":0,"speed":3})json")), ValidationError);
  EXPECT_THROW(store_.ValidatePrimitive("base.navigate", "1.0.0",
               Wire(R"json({"type":"base.navigate","reference":"relative",
                     "x_m":1,"y_m":2,"yaw_rad":0,"z_m":0})json")), ValidationError);
  store_.ValidatePrimitive("gripper.motion", "1.0.0", Wire(R"json({"type": "gripper.motion",
                                             "command_id": "cmd-1045",
                                             "action": "close", "force_n": 5.0})json"));
  store_.ValidatePrimitive("gripper.motion", "1.0.0", Wire(R"json({"type": "gripper.motion",
                                             "command_id": "cmd-2045", "action": "open",
                                             "opening_pct": 100.0, "speed_pct": 50.0})json"));
  store_.ValidatePrimitive("speaker.play", "1.0.0", Wire(R"json({"type": "speaker.play",
                                             "uri": "asset://alerts/ready.wav"})json"));
  store_.ValidatePrimitive("speaker.speak", "1.0.0", Wire(R"json({"type": "speaker.speak",
                                              "text": "ready", "language": "en"})json"));
  store_.ValidatePrimitive("arm.movej", "1.0.0", Wire(R"json({"type": "arm.movej",
                                             "joint_positions_rad": [0.1, 0.2, 0.3],
                                             "speed_rad_s": 0.5, "reference": "absolute"})json"));
  store_.ValidatePrimitive("arm.pick", "1.0.0",
                           Wire(R"json({"type": "arm.pick", "object_id": "obj:part-7"})json"));
  EXPECT_THROW(
      store_.ValidatePrimitive("arm.pick", "1.0.0",
                               Wire(R"json({"type": "arm.pick", "object_id": "human:7"})json")),
      ValidationError);
  store_.ValidatePrimitive("motion.stop", "1.0.0", Wire(R"json({"type": "motion.stop",
                                                 "command_id": null})json"));
}

TEST_F(SchemaStoreTest, SpeakerPlayRejectsTextAndStreamChunks) {
  EXPECT_THROW(store_.ValidatePrimitive("speaker.play", "1.0.0", Wire(R"json({"type":"speaker.play",
                                    "text":"legacy speech"})json")),
               ValidationError);
  EXPECT_THROW(store_.ValidatePrimitive("speaker.play", "1.0.0", Wire(R"json({"type":"speaker.play",
                                    "stream_id":"bad","sequence":0,
                                    "media_type":"audio/wav","data_b64":"not-base64",
                                    "end_of_stream":true})json")),
               ValidationError);
}

TEST_F(SchemaStoreTest, PrimitiveSchemasDeferNumericRangesToRuntimeAdmission) {
  EXPECT_NO_THROW(store_.ValidatePrimitive(
      "base.velocity", "1.0.0", Wire(R"json({"type": "base.velocity", "linear_x_m_s": 42.0,
                    "linear_y_m_s": -17.0, "angular_z_rad_s": 9.0,
                    "valid_for_ms": 5000})json")));
  EXPECT_NO_THROW(store_.ValidatePrimitive(
      "base.navigate", "1.0.0", Wire(R"json({"type": "base.navigate", "reference": "relative",
                    "x_m": 42.0, "y_m": -17.0, "yaw_rad": 9.0})json")));
  EXPECT_NO_THROW(store_.ValidatePrimitive(
      "gimbal.motion", "1.0.0", Wire(R"json({"type": "gimbal.motion", "reference": "absolute",
                    "pan_rad": 10.0, "tilt_rad": -10.0, "speed_rad_s": -1.0})json")));
}

TEST_F(SchemaStoreTest, RejectsWorldSnapshotMissingSequence) {
  const std::vector<SchemaIssue> issues = ExpectRejected([this] {
    store_.ValidateResource("world.snapshot", Wire(R"json({
      "type":"world.snapshot","schema_version":"1.0",
      "timestamp":"2026-09-21T00:00:00Z","frame_id":"map","entities":[],"relations":[]
    })json"));
  });
  ASSERT_EQ(issues.size(), 1u);
  EXPECT_EQ(issues[0].instance_path, "");
  EXPECT_EQ(issues[0].schema_path, "#/required");
  EXPECT_NE(issues[0].message.find("'sequence'"), std::string::npos);
}

TEST_F(SchemaStoreTest, RejectsInvalidDetectionIdentityForKind) {
  const std::vector<SchemaIssue> issues = ExpectRejected([this] {
    store_.ValidateResource("detection.snapshot", Wire(R"json({
      "type":"detection.snapshot","schema_version":"1.0","sequence":1,
      "timestamp":"2026-09-21T00:00:00Z","source":"cam-0",
      "detections":[{"id":"obj:7","kind":"human","label":"person","confidence":0.9}]
    })json"));
  });
  ASSERT_GE(issues.size(), 1u);
  EXPECT_EQ(issues.front().instance_path, "/detections/0/id");
}

TEST_F(SchemaStoreTest, RejectsBaseVelocityWithoutRequiredSetpoint) {
  const std::vector<SchemaIssue> issues = ExpectRejected([this] {
    store_.ValidatePrimitive("base.velocity", "1.0.0", Wire(R"json({"type": "base.velocity",
                                               "command_id": "cmd-1041", "linear_x_m_s": 0.2})json"));
  });
  ASSERT_GE(issues.size(), 1u);
  EXPECT_EQ(issues[0].schema_path, "#/required");
  EXPECT_FALSE(issues[0].message.empty());
}

TEST_F(SchemaStoreTest, RejectsAckMissingAccepted) {
  const std::vector<SchemaIssue> issues = ExpectRejected([this] {
    store_.ValidateAck(Wire(
        R"json({"type": "primitive.ack", "primitive_version": "1.0.0", "primitive": "arm.movel",
                                   "reason": null, "command_id": "cmd-1041", "sequence": 413})json"));
  });
  ASSERT_GE(issues.size(), 1u);
  EXPECT_NE(issues.front().message.find("'accepted'"), std::string::npos);
}

TEST_F(SchemaStoreTest, UnknownResourceTypeNamesKnownSet) {
  const std::vector<SchemaIssue> issues =
      ExpectRejected([this] { store_.ValidateResource("unknown.snapshot", Wire("{}")); });
  ASSERT_EQ(issues.size(), 1u);
  EXPECT_NE(issues[0].message.find("unknown.snapshot"), std::string::npos);
  EXPECT_NE(issues[0].message.find("asr.snapshot"), std::string::npos);
  EXPECT_NE(issues[0].message.find("vision.snapshot"), std::string::npos);
}

TEST_F(SchemaStoreTest, UnknownPrimitiveIdentityNamesKnownSet) {
  const std::vector<SchemaIssue> issues =
      ExpectRejected([this] { store_.ValidatePrimitive("arm.wave", "1.0.0", Wire("{}")); });
  ASSERT_EQ(issues.size(), 1u);
  EXPECT_NE(issues[0].message.find("arm.wave"), std::string::npos);
  EXPECT_NE(issues[0].message.find("base.velocity"), std::string::npos);
  EXPECT_NE(issues[0].message.find("gripper.motion"), std::string::npos);
}

TEST_F(SchemaStoreTest, RemovedCompositeAndReleaseDiscriminatorsAreUnknown) {
  for (const char* removed :
       {"arm.drop", "arm.stow", "base.move", "base.turn", "gimbal.turn", "safety.estop.release"}) {
    const std::vector<SchemaIssue> issues =
        ExpectRejected([this, removed] { store_.ValidatePrimitive(removed, "1.0.0", Wire("{}")); });
    ASSERT_EQ(issues.size(), 1u) << removed;
    EXPECT_NE(issues[0].message.find("unknown primitive"), std::string::npos) << removed;
  }
}

TEST_F(SchemaStoreTest, LegacyPartTargetInsideArgumentsIsRejected) {
  const std::vector<SchemaIssue> issues = ExpectRejected([this] {
    store_.ValidatePrimitive(
        "base.posture", "1.0.0",
        Wire(R"json({"type":"base.posture","posture":"stand","part_id":"default"})json"));
  });
  ASSERT_GE(issues.size(), 1u);
  EXPECT_EQ(issues.front().schema_path, "#/additionalProperties");
  EXPECT_NE(issues.front().message.find("part_id"), std::string::npos);
}

TEST_F(SchemaStoreTest, UnknownSchemaIdNamesKnownSet) {
  const std::vector<SchemaIssue> issues =
      ExpectRejected([this] { store_.ValidateById("egia.contracts.nope.v1", Wire("{}")); });
  ASSERT_EQ(issues.size(), 1u);
  EXPECT_NE(issues[0].message.find("egia.contracts.nope.v1"), std::string::npos);
  EXPECT_NE(issues[0].message.find("egia.contracts.actions.ack.v1"), std::string::npos);
}

/// Scratch contracts root for load-failure cases; a fresh tree per test.
class ContractLoadTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / "eos_northbound_contracts_test";
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "actions");
    std::filesystem::create_directories(root_ / "body");
    std::filesystem::create_directories(root_ / "world");
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  /// Writes `text` to `<root>/<rel>`, creating parent directories on demand.
  std::filesystem::path Write(const std::string& rel, const std::string& text) {
    const std::filesystem::path file = root_ / rel;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << text;
    return file;
  }

  /// A minimal compiling schema with the given `$id` and type const.
  std::string Schema(const std::string& id, const std::string& type_const) const {
    return R"({"$schema": "https://json-schema.org/draft/2020-12/schema",)"
           R"("$id": "test.)" +
           id + R"(.v1", )" +
           (type_const == "primitive.ack" ? "" : R"("x-island-primitive-version": "1.0.0", )") +
           R"("type": "object", "properties": )" + R"({"type": {"const": ")" + type_const +
           R"("}}})";
  }

  void CopyCanonicalContracts() {
    std::filesystem::remove_all(root_);
    std::filesystem::copy(std::filesystem::path(EOS_CONTRACTS_ROOT) / "ext/eos", root_,
                          std::filesystem::copy_options::recursive);
  }

  std::filesystem::path root_;
};

TEST_F(ContractLoadTest, MissingManifestFailsProductionLoad) {
  try {
    SchemaStore store(root_);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("contracts.manifest.json"), std::string::npos);
  }
}

TEST_F(ContractLoadTest, ManifestRejectsRolePathMismatch) {
  CopyCanonicalContracts();
  const std::filesystem::path manifest_path = root_ / "contracts.manifest.json";
  std::ifstream input(manifest_path);
  nlohmann::json manifest = nlohmann::json::parse(input);
  manifest["schemas"][0]["role"] = "world-resource";
  std::ofstream(manifest_path) << manifest.dump(2);

  try {
    SchemaStore store(root_);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("role/path mismatch"), std::string::npos);
    EXPECT_NE(what.find("acknowledgement"), std::string::npos);
  }
}

TEST_F(ContractLoadTest, ManifestRejectsStaleSchemaHash) {
  CopyCanonicalContracts();
  const std::filesystem::path schema = root_ / "body/base.v1.json";
  std::ofstream(schema, std::ios::app) << "\n";

  try {
    SchemaStore store(root_);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("content hash differs"), std::string::npos);
    EXPECT_NE(what.find("base.v1.json"), std::string::npos);
  }
}

TEST_F(ContractLoadTest, MissingTreeNamesRoot) {
  std::filesystem::remove_all(root_ / "world");
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("world"), std::string::npos);
    EXPECT_NE(std::string(e.what()).find(root_.string()), std::string::npos);
  }
}

TEST_F(ContractLoadTest, InvalidJsonNamesFile) {
  Write("actions/ack.v1.json", Schema("ack", "primitive.ack"));
  const std::filesystem::path broken = Write("actions/zz_broken.v1.json", "{ not json");
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find(broken.filename().string()), std::string::npos);
    EXPECT_NE(what.find("valid JSON"), std::string::npos);
  }
}

TEST_F(ContractLoadTest, MissingExternalReferenceFailsStartup) {
  Write("actions/ack.v1.json", Schema("ack", "primitive.ack"));
  const std::filesystem::path broken =
      Write("world/broken.v1.json",
            R"json({"$schema":"https://json-schema.org/draft/2020-12/schema",
               "$id":"test.broken.v1","type":"object",
               "properties":{"type":{"const":"broken.snapshot"},
                             "state":{"$ref":"test.missing.v1#/$defs/State"}}})json");
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    EXPECT_NE(std::string(e.what()).find(broken.filename().string()), std::string::npos);
    EXPECT_NE(std::string(e.what()).find("test.missing.v1"), std::string::npos);
  }
}

TEST_F(ContractLoadTest, UnreadableFileNamesFile) {
  Write("actions/ack.v1.json", Schema("ack", "primitive.ack"));
  const std::filesystem::path file = Write("actions/zz_unreadable.v1.json", "{}");
  std::filesystem::permissions(file, std::filesystem::perms::none);
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    EXPECT_NE(std::string(e.what()).find(file.filename().string()), std::string::npos);
  }
}

TEST_F(ContractLoadTest, DuplicatePrimitiveTypeNamesBothFiles) {
  Write("actions/ack.v1.json", Schema("ack", "primitive.ack"));
  Write("actions/a_one.v1.json", Schema("one", "base.velocity"));
  const std::filesystem::path second =
      Write("actions/b_two.v1.json", Schema("two", "base.velocity"));
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("duplicate primitive identity 'base.velocity@1.0.0'"), std::string::npos);
    EXPECT_NE(what.find(second.filename().string()), std::string::npos);
  }
}

TEST_F(ContractLoadTest, EmptyTreeRefusesWithoutAck) {
  try {
    SchemaStore store(root_, false);
    FAIL() << "expected ContractLoadError";
  } catch (const ContractLoadError& e) {
    EXPECT_NE(std::string(e.what()).find("primitive.ack"), std::string::npos);
  }
}

}  // namespace
}  // namespace eos::runtime::init

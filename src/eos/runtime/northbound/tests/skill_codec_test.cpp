#include "src/eos/runtime/northbound/skill_codec.h"

#include <gtest/gtest.h>

namespace eos::runtime::northbound {
namespace {

TEST(SkillCodecTest, ExposesSourceAndIpcChannelDeclarations) {
  skills::Metadata metadata;
  metadata.name = "test.process";
  metadata.version = "1.0.0";
  metadata.parameters_schema = {{"type", "object"}};
  metadata.returns_schema = {{"type", "object"}};
  metadata.sources.push_back(
      {"vision.people", {{"type", "object"}}, skills::SourceAuthority::kRead});
  metadata.inference_authorities = {skills::InferenceAuthority::kVad,
                                    skills::InferenceAuthority::kHuman,
                                    skills::InferenceAuthority::kHumanPose,
                                    skills::InferenceAuthority::kFaceRecognition,
                                    skills::InferenceAuthority::kChatCompletion,
                                    skills::InferenceAuthority::kVlm};
  metadata.ipc_channels.push_back(
      {"control", {{"type", "object"}}, true, true});
  metadata.execution_state_schema = {
      {"type", "object"}, {"properties", {{"phase", {{"type", "string"}}}}}};
  metadata.related_process_state_schema = {
      {"type", "object"}, {"properties", {{"identity", {{"type", "string"}}}}}};

  island::eos::v1::SkillDescriptor encoded;
  ASSERT_TRUE(FillSkillDescriptor(metadata, &encoded).ok());
  ASSERT_EQ(encoded.sources_size(), 1);
  EXPECT_EQ(encoded.sources(0).topic(), "vision.people");
  EXPECT_EQ(encoded.sources(0).authority(), island::eos::v1::SKILL_SOURCE_AUTHORITY_READ);
  EXPECT_EQ(encoded.sources(0).schema().fields().at("type").string_value(), "object");
  ASSERT_EQ(encoded.inference_authorities_size(), 6);
  EXPECT_EQ(encoded.inference_authorities(0),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_VAD);
  EXPECT_EQ(encoded.inference_authorities(1),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_HUMAN);
  EXPECT_EQ(encoded.inference_authorities(2),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_HUMAN_POSE);
  EXPECT_EQ(encoded.inference_authorities(3),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_FACE_RECOGNITION);
  EXPECT_EQ(encoded.inference_authorities(4),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_CHAT_COMPLETION);
  EXPECT_EQ(encoded.inference_authorities(5),
            island::eos::v1::SKILL_INFERENCE_AUTHORITY_VLM);
  ASSERT_EQ(encoded.ipc_channels_size(), 1);
  EXPECT_EQ(encoded.ipc_channels(0).name(), "control");
  EXPECT_TRUE(encoded.ipc_channels(0).can_send());
  EXPECT_TRUE(encoded.ipc_channels(0).can_receive());
  EXPECT_EQ(encoded.ipc_channels(0).payload_schema().fields().at("type").string_value(),
            "object");
  EXPECT_EQ(encoded.execution_state_schema().fields().at("type").string_value(), "object");
  EXPECT_EQ(encoded.GetDescriptor()->FindFieldByName("related_process_state_schema"), nullptr);
}

TEST(SkillCodecTest, ExposesProcessParentTimingAndGeneralActiveClaims) {
  skills::ExecutionSnapshot snapshot;
  snapshot.execution_id = "child";
  snapshot.parent_execution_id = "parent";
  snapshot.skill_name = "test.process";
  snapshot.skill_version = "1.0.0";
  snapshot.boot_id = "boot";
  snapshot.event_time.source_timestamp_ns = 42;
  snapshot.event_time.source_clock_id = "device.camera";
  snapshot.event_time.boot_id = "boot";
  snapshot.event_time.sequence = 7;
  snapshot.event_time.event_id = "event";
  snapshot.active_device_claims.push_back(
      {skills::ClaimKind::kDevice, skills::ClaimMode::kExclusiveUse, "arm-0"});

  island::eos::v1::SkillExecution encoded;
  ASSERT_TRUE(FillSkillExecution(snapshot, &encoded).ok());
  EXPECT_EQ(encoded.parent_execution_id(), "parent");
  EXPECT_FALSE(encoded.has_deadline_at());
  EXPECT_EQ(encoded.event_time().source_timestamp_ns(), 42);
  EXPECT_EQ(encoded.event_time().source_clock_id(), "device.camera");
  EXPECT_EQ(encoded.event_time().sequence(), 7u);
  ASSERT_EQ(encoded.active_claims_size(), 1);
  EXPECT_EQ(encoded.active_claims(0).identity(), "arm-0");
  EXPECT_EQ(encoded.active_device_claims_size(), 1);
}

TEST(SkillCodecTest, EncodesOnlyPublicRetainedExecutionState) {
  skills::RetainedExecutionState state;
  state.revision = 4;
  state.published_at = std::chrono::system_clock::time_point(std::chrono::seconds(12));
  state.payload = {{"phase", "tracking"}};

  island::eos::v1::SkillExecutionState encoded;
  ASSERT_TRUE(FillExecutionState("execution-1", state, &encoded).ok());
  EXPECT_EQ(encoded.execution_id(), "execution-1");
  EXPECT_EQ(encoded.revision(), 4u);
  EXPECT_EQ(encoded.published_at().seconds(), 12);
  EXPECT_EQ(encoded.payload().fields().at("phase").string_value(), "tracking");
}

TEST(SkillCodecTest, NormalizesPreEpochTimestamps) {
  skills::RetainedExecutionState state;
  state.revision = 1;
  state.published_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(-250));
  state.payload = nlohmann::json::object();

  island::eos::v1::SkillExecutionState encoded;
  ASSERT_TRUE(FillExecutionState("execution-1", state, &encoded).ok());
  EXPECT_EQ(encoded.published_at().seconds(), -1);
  EXPECT_EQ(encoded.published_at().nanos(), 750000000);
}

}  // namespace
}  // namespace eos::runtime::northbound

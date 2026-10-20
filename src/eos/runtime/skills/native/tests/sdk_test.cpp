#include <cstddef>
#include <cstring>
#include <eos/skill/sdk.hpp>
#include <string>

#include "gtest/gtest.h"

namespace {

using eos::skill::CheckpointOptions;
using eos::skill::Execution;
using eos::skill::Json;
using eos::skill::Library;
using eos::skill::LibraryBuilder;
using eos::skill::Outcome;
using eos::skill::SkillBuilder;

eos_skill_text_v2 Borrow(const std::string& value) {
  return {value.data(), static_cast<std::uint32_t>(value.size())};
}

struct FakeHost {
  eos_skill_host_v2 abi{};
  std::string log_event;
  std::string checkpoint_name;
  std::string failure_reason;
  std::string failure_message;
  std::string primitive_acknowledgement;
  std::string primitive_error;
  std::uint64_t operation_id{0};
  bool primitive_completed{false};
  bool primitive_cancelled{false};
  std::string source_topic;
  std::string source_value;
  std::string inference_result;
  std::size_t inference_calls{0};
  int completions{0};

  FakeHost() {
    abi.abi_version = EOS_SKILL_ABI_VERSION_V2;
    abi.struct_size = sizeof(abi);
    abi.context = this;
    abi.is_cancelled = [](void*) { return 0u; };
    abi.log = [](void* opaque, const eos_skill_log_record_v2* record) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->log_event.assign(record->event_name.data, record->event_name.size);
      return EOS_SKILL_HOST_OK;
    };
    abi.emit_checkpoint = [](void* opaque, const eos_skill_checkpoint_v2* checkpoint) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->checkpoint_name.assign(checkpoint->name.data, checkpoint->name.size);
      return EOS_SKILL_HOST_OK;
    };
    abi.report_failure = [](void* opaque, const eos_skill_failure_v2* failure) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->failure_reason.assign(failure->reason_code.data, failure->reason_code.size);
      self->failure_message.assign(failure->message.data, failure->message.size);
      return EOS_SKILL_HOST_OK;
    };
    abi.start_primitive = [](void* opaque, const eos_skill_primitive_request_v2*,
                             eos_skill_primitive_start_response_v2* response) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->operation_id = 7;
      response->operation_id = self->operation_id;
      return EOS_SKILL_HOST_OK;
    };
    abi.wait_primitive = [](void* opaque, std::uint64_t operation_id, std::uint32_t,
                            eos_skill_primitive_wait_response_v2* response) {
      auto* self = static_cast<FakeHost*>(opaque);
      if (operation_id != self->operation_id) return EOS_SKILL_HOST_NOT_FOUND;
      if (!self->primitive_completed) return EOS_SKILL_HOST_OK;
      self->primitive_acknowledgement =
          R"({"accepted":true,"result":{"status":"succeeded"}})";
      response->completed = 1;
      response->acknowledgement_json = Borrow(self->primitive_acknowledgement);
      return EOS_SKILL_HOST_OK;
    };
    abi.cancel_primitive = [](void* opaque, std::uint64_t operation_id) {
      auto* self = static_cast<FakeHost*>(opaque);
      if (operation_id != self->operation_id) return EOS_SKILL_HOST_NOT_FOUND;
      self->primitive_cancelled = true;
      return EOS_SKILL_HOST_OK;
    };
    abi.publish_source = [](void* opaque, const eos_skill_source_sample_v2* sample,
                            eos_skill_source_response_v2* response) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->source_topic.assign(sample->topic.data, sample->topic.size);
      self->source_value.assign(sample->value_json.data, sample->value_json.size);
      response->has_sample = 1;
      response->sample = *sample;
      response->sample.sequence = 9;
      return EOS_SKILL_HOST_OK;
    };
    abi.query_source = [](void* opaque, const eos_skill_source_query_v2* query,
                          eos_skill_source_response_v2* response) {
      auto* self = static_cast<FakeHost*>(opaque);
      self->source_topic.assign(query->topic.data, query->topic.size);
      self->source_value = R"({"value":7})";
      response->has_sample = 1;
      response->sample = {sizeof(response->sample), Borrow(self->source_topic), 42, 10,
                          Borrow(self->source_value)};
      return EOS_SKILL_HOST_OK;
    };
    abi.infer_once = [](void* opaque, const eos_skill_inference_request_v2* request,
                        eos_skill_inference_response_v2* response) {
      auto* self = static_cast<FakeHost*>(opaque);
      ++self->inference_calls;
      if (request->authority == EOS_SKILL_INFERENCE_VAD) {
        EXPECT_EQ(request->source.size, 0u);
        EXPECT_EQ(request->source_sequence, 0u);
        EXPECT_EQ(request->source_generation, 0u);
      } else if (request->source.size != 0) {
        EXPECT_EQ(request->authority, EOS_SKILL_INFERENCE_HUMAN_POSE);
        EXPECT_EQ(request->source_sequence, 12u);
        EXPECT_EQ(request->source_generation, 9u);
      } else {
        EXPECT_EQ(request->authority, EOS_SKILL_INFERENCE_HUMAN_POSE);
        EXPECT_EQ(request->source_sequence, 0u);
        EXPECT_EQ(request->source_generation, 0u);
      }
      self->inference_result = R"({"poses":[{"score":0.9}]})";
      response->result_json = Borrow(self->inference_result);
      return EOS_SKILL_HOST_OK;
    };
  }
};

eos_skill_execution_v2 Invocation(FakeHost& host, const std::string& arguments) {
  static const std::string command = "sdk-test";
  return {sizeof(eos_skill_execution_v2), Borrow(command), Borrow(arguments), &host.abi};
}

Outcome SuccessfulHandler(Execution& execution) {
  EXPECT_EQ(execution.arguments().at("value"), 7);
  EXPECT_TRUE(execution.logger().info("sdk_test", "running", {{"count", 1}}));
  EXPECT_TRUE(
      execution.checkpoint("sdk.running", CheckpointOptions().Progress(0.5).Fields({{"step", 1}})));
  return Outcome::Success("completed", {{"ok", true}}, "work completed");
}

Outcome RejectedHandler(Execution&) {
  return Outcome::Reject("target.missing", {{"ok", false}}, "target was not found");
}

Outcome InvalidHandler(Execution&) {
  return Outcome::InvalidArgument("arguments.unsupported", "mode is not supported");
}

Outcome FailedHandler(Execution&) {
  return Outcome::Failure("scan.motion_failed", "gimbal motion failed");
}

Outcome ThrowingHandler(Execution&) { throw std::runtime_error("private details"); }

eos_skill_host_status_v2 Complete(void* opaque, const eos_skill_completion_v2* completion) {
  auto* host = static_cast<FakeHost*>(opaque);
  ++host->completions;
  EXPECT_EQ(completion->accepted, 1U);
  const std::string result(completion->result_json.data, completion->result_json.size);
  EXPECT_EQ(Json::parse(result), Json({{"ok", true}}));
  EXPECT_TRUE(EOS_SKILL_COMPLETION_V2_HAS(completion, message));
  EXPECT_EQ(std::string(completion->message.data, completion->message.size), "work completed");
  return EOS_SKILL_HOST_OK;
}

TEST(SkillSdkTest, RunsTypedHandlerAndOptionalObservability) {
  FakeHost host;
  const std::string arguments = R"({"value":7})";
  auto invocation = Invocation(host, arguments);
  EXPECT_EQ(eos::skill::Run(&SuccessfulHandler, &invocation, &Complete, &host),
            EOS_SKILL_EXECUTE_OK);
  EXPECT_EQ(host.completions, 1);
  EXPECT_EQ(host.log_event, "sdk_test");
  EXPECT_EQ(host.checkpoint_name, "sdk.running");
}

TEST(SkillSdkTest, ReportsTypedNonOkOutcomes) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);

  EXPECT_EQ(eos::skill::Run(&InvalidHandler, &invocation, &Complete, &host),
            EOS_SKILL_EXECUTE_INVALID_ARGUMENT);
  EXPECT_EQ(host.failure_reason, "arguments.unsupported");
  EXPECT_EQ(host.failure_message, "mode is not supported");

  host.failure_reason.clear();
  host.failure_message.clear();
  EXPECT_EQ(eos::skill::Run(&FailedHandler, &invocation, &Complete, &host),
            EOS_SKILL_EXECUTE_FAILED);
  EXPECT_EQ(host.failure_reason, "scan.motion_failed");
  EXPECT_EQ(host.failure_message, "gimbal motion failed");

  host.failure_reason.clear();
  host.failure_message.clear();
  EXPECT_EQ(eos::skill::Run(&ThrowingHandler, &invocation, &Complete, &host),
            EOS_SKILL_EXECUTE_FAILED);
  EXPECT_EQ(host.failure_reason, "skill.exception");
  EXPECT_EQ(host.failure_message, "skill handler threw an exception");
  EXPECT_EQ(host.failure_message.find("private details"), std::string::npos);
}

eos_skill_host_status_v2 CompleteRejected(void* opaque, const eos_skill_completion_v2* completion) {
  auto* host = static_cast<FakeHost*>(opaque);
  ++host->completions;
  EXPECT_EQ(completion->accepted, 0U);
  EXPECT_EQ(std::string(completion->reason_code.data, completion->reason_code.size),
            "target.missing");
  EXPECT_EQ(std::string(completion->message.data, completion->message.size),
            "target was not found");
  return EOS_SKILL_HOST_OK;
}

TEST(SkillSdkTest, AddsMessageToRejectedCompletion) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  EXPECT_EQ(eos::skill::Run(&RejectedHandler, &invocation, &CompleteRejected, &host),
            EOS_SKILL_EXECUTE_OK);
  EXPECT_EQ(host.completions, 1);
}

TEST(SkillSdkTest, StartsPollsAndCancelsPrimitiveOperation) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);

  const auto started = execution.start_primitive({"gimbal.motion", "1.0.0"}, "gimbal-0",
                                                 {{"pan_rad", 0.5}});
  ASSERT_TRUE(started);
  EXPECT_EQ(started.value().id, 7u);
  const auto pending = execution.wait_primitive(started.value(), std::chrono::milliseconds(0));
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending.value().has_value());

  EXPECT_TRUE(execution.cancel_primitive(started.value()));
  EXPECT_TRUE(host.primitive_cancelled);
  host.primitive_completed = true;
  const auto completed = execution.wait_primitive(started.value(), std::chrono::milliseconds(20));
  ASSERT_TRUE(completed);
  ASSERT_TRUE(completed.value().has_value());
  EXPECT_EQ(completed.value()->at("result").at("status"), "succeeded");
}

TEST(SkillSdkTest, PublishesAndQueriesTimestampedSources) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  const auto published = execution.publish_source("vision.people", 42, {{"value", 7}});
  ASSERT_TRUE(published);
  EXPECT_EQ(published.value().sequence, 9u);
  const auto queried = execution.query_source("vision.people", eos::skill::SourceLookup::kExact, 42);
  ASSERT_TRUE(queried);
  ASSERT_TRUE(queried.value());
  EXPECT_EQ(queried.value()->value.at("value"), 7);
}

TEST(SkillSdkTest, RunsTypedInferenceForExactRetainedFrame) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  const auto inferred = execution.infer_once<eos::skill::HumanPoseInference>(
      {"camera.front", 12, 9}, {{"human_id", "human:7"}});
  ASSERT_TRUE(inferred);
  EXPECT_EQ(inferred.value().at("poses").size(), 1u);
}

TEST(SkillSdkTest, RunsVisionInferenceAgainstCallerSuppliedCanonicalMedia) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  const auto inferred = execution.infer_once<eos::skill::HumanPoseInference>(
      {{"input", {{"data_b64", "AAAA"}}},
       {"persons", {{{"track_id", 7}, {"bbox", {1, 2, 3, 4}}, {"confidence", 0.9}}}}});
  ASSERT_TRUE(inferred);
  EXPECT_EQ(host.inference_calls, 1u);
}

TEST(SkillSdkTest, RejectsMixedVisionInputModeBeforeHostCall) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  EXPECT_FALSE(execution.infer_once<eos::skill::HumanPoseInference>(
      {"camera.front", 12, 9}, {{"input", {{"data_b64", "AAAA"}}}}));
  EXPECT_EQ(host.inference_calls, 0u);
}

TEST(SkillSdkTest, RunsVadAgainstCallerSuppliedCanonicalAudioWithoutProvenance) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  const auto inferred = execution.infer_once<eos::skill::VadInference>(
      {{"input", {{"data_b64", "AAAA"}, {"sample_rate_hz", 16000}, {"channels", 1}}}});
  ASSERT_TRUE(inferred);
  EXPECT_EQ(host.inference_calls, 1u);
}

TEST(SkillSdkTest, RejectsVadClaimedProvenanceAndMissingInputBeforeHostCall) {
  FakeHost host;
  const std::string arguments = "{}";
  auto invocation = Invocation(host, arguments);
  Execution execution(invocation);
  EXPECT_FALSE(execution.infer_once<eos::skill::VadInference>(
      {"microphone.front", 1, 1}, {{"input", {{"data_b64", "AAAA"}}}}));
  EXPECT_FALSE(execution.infer_once<eos::skill::VadInference>(nlohmann::json::object()));
  EXPECT_EQ(host.inference_calls, 0u);
}

TEST(SkillSdkTest, DoesNotReadOptionalMembersFromOldHost) {
  struct OldHost {
    std::uint32_t abi_version;
    std::uint32_t struct_size;
    void* context;
    eos_skill_call_primitive_v2 call_primitive;
    eos_skill_is_cancelled_v2 is_cancelled;
    eos_skill_read_resource_v2 read_resource;
    eos_skill_receive_signal_v2 receive_signal;
    eos_skill_emit_signal_v2 emit_signal;
  } old{};
  static_assert(sizeof(OldHost) == EOS_SKILL_HOST_V2_BASE_SIZE);
  old.abi_version = EOS_SKILL_ABI_VERSION_V2;
  old.struct_size = sizeof(old);
  const std::string command = "old-host";
  const std::string arguments = "{}";
  eos_skill_execution_v2 invocation{sizeof(invocation), Borrow(command), Borrow(arguments),
                                    reinterpret_cast<const eos_skill_host_v2*>(&old)};
  Execution execution(invocation);
  EXPECT_FALSE(execution.logger().info("optional_missing"));
  EXPECT_FALSE(execution.checkpoint("optional.missing"));
  EXPECT_FALSE(execution.start_primitive({"gimbal.motion", "1.0.0"}, "gimbal-0", {}));
  EXPECT_EQ(eos::skill::Run(&FailedHandler, &invocation, &Complete, nullptr),
            EOS_SKILL_EXECUTE_FAILED);
}

TEST(SkillSdkTest, BuilderOwnsStableAbiStorageAcrossMoves) {
  Library library =
      std::move(LibraryBuilder("example.library", "1.0.0", "build")
                    .Skill(SkillBuilder("example.skill", "1.0.0", "Example", {{"type", "object"}},
                                        {{"type", "object"}}, &SuccessfulHandler)
                               .Primitive("speaker.speak", "1.0.0")
                                .Resource("robots/{robot}/state", "State")
                                .Source("vision.people", {{"type", "object"}},
                                        eos::skill::SourceAuthority::kRead)
                                .Inference(eos::skill::InferenceAuthority::kHumanPose)
                                .Inference(eos::skill::InferenceAuthority::kChatCompletion)
                                .Inference(eos::skill::InferenceAuthority::kVlm)
                                .Reentrant())
                    .Build());
  ASSERT_NE(library.abi(), nullptr);
  ASSERT_EQ(library.abi()->skill_count, 1U);
  const auto& descriptor = library.abi()->skills[0];
  EXPECT_EQ(std::string(descriptor.name.data, descriptor.name.size), "example.skill");
  ASSERT_EQ(descriptor.primitive_count, 1U);
  EXPECT_EQ(std::string(descriptor.primitives[0].name.data, descriptor.primitives[0].name.size),
             "speaker.speak");
  ASSERT_EQ(descriptor.source_count, 1U);
  ASSERT_EQ(descriptor.inference_count, 3U);
  EXPECT_EQ(descriptor.inferences[0].authority, EOS_SKILL_INFERENCE_HUMAN_POSE);
  EXPECT_EQ(descriptor.inferences[1].authority, EOS_SKILL_INFERENCE_CHAT_COMPLETION);
  EXPECT_EQ(descriptor.inferences[2].authority, EOS_SKILL_INFERENCE_VLM);
}

TEST(SkillSdkTest, RejectsInvalidArgumentsAndProgress) {
  FakeHost host;
  const std::string arguments = "[]";
  auto invocation = Invocation(host, arguments);
  EXPECT_EQ(eos::skill::Run(&SuccessfulHandler, &invocation, &Complete, &host),
            EOS_SKILL_EXECUTE_FAILED);

  const std::string valid = "{}";
  invocation = Invocation(host, valid);
  Execution execution(invocation);
  EXPECT_FALSE(execution.checkpoint("bad.progress", CheckpointOptions().Progress(2.0)));
}

}  // namespace

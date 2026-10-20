#include "src/eos/runtime/skills/native/runtime.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/eos/ext/backends/demo/gimbal/demo_gimbal.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/skills/manager/execution.h"
#include "src/eos/runtime/skills/manager/logging.h"
#include "src/eos/runtime/skills/native/primitive_ack.h"
#include "tests/eos/in_process_simulator.h"

namespace eos::runtime::skills::native {
namespace {
class SkillLogSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<std::string> messages;
  std::atomic<std::size_t> lifecycle_count{0};

  /// Terminal records are logged after the execution state turns terminal, so a
  /// test that needs one must poll for it instead of reading once. Locked
  /// against concurrent sink_it_ writes.
  bool HasLineStartingWith(const std::string& prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(messages.begin(), messages.end(), [&](const std::string& message) {
      return message.rfind(prefix, 0) == 0;
    });
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& entry) override {
    messages.emplace_back(entry.payload.data(), entry.payload.size());
    if (messages.back().find("[skill_execution_") != std::string::npos)
      lifecycle_count.fetch_add(1);
  }
  void flush_() override {}
};

class SkillRuntimeTest : public ::testing::Test {
 protected:
  Runtime LoadFixture() { return Runtime::LoadDirectory(EOS_TEST_SKILL_DIR, dispatcher_); }

  test::InProcessSimulator simulator_;
  runtime::init::SchemaStore schemas_{EOS_CONTRACTS_ROOT};
  runtime::hardware_runtime::Registry registry_{runtime::hardware_runtime::Registry::Adopt(
      runtime::hardware_runtime::LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml",
                                             "demo"),
      "", simulator_.socket_path())};
  dispatch::Dispatcher dispatcher_{registry_, schemas_};
};


TEST(PrimitiveAcknowledgementTest, AcceptsCanonicalTerminalOutcomes) {
  using detail::ParsePrimitiveAcknowledgement;
  using detail::PrimitiveTerminalStatus;

  EXPECT_EQ(ParsePrimitiveAcknowledgement(
                {{"accepted", true}, {"reason", nullptr}, {"result", {{"status", "succeeded"}}}})
                .status,
            PrimitiveTerminalStatus::kSucceeded);

  const detail::PrimitiveAcknowledgement failed =
      ParsePrimitiveAcknowledgement({{"accepted", true},
                                     {"reason", "backend could not reach the admitted goal"},
                                     {"result", {{"status", "failed"}}}});
  EXPECT_EQ(failed.status, PrimitiveTerminalStatus::kFailed);
  EXPECT_EQ(failed.message, "backend could not reach the admitted goal");

  EXPECT_EQ(
      ParsePrimitiveAcknowledgement(
          {{"accepted", false}, {"reason", "device is busy"}, {"result", {{"status", "rejected"}}}})
          .status,
      PrimitiveTerminalStatus::kRejected);
  EXPECT_EQ(ParsePrimitiveAcknowledgement(
                {{"accepted", true}, {"result", {{"status", "cancelled"}}}}).status,
            PrimitiveTerminalStatus::kCancelled);
  EXPECT_EQ(ParsePrimitiveAcknowledgement(
                {{"accepted", false}, {"result", {{"status", "cancelled"}}}}).status,
            PrimitiveTerminalStatus::kCancelled);
  EXPECT_EQ(ParsePrimitiveAcknowledgement(
                {{"accepted", true}, {"result", {{"status", "timed_out"}}}}).status,
            PrimitiveTerminalStatus::kTimedOut);
  EXPECT_EQ(ParsePrimitiveAcknowledgement(
                {{"accepted", true}, {"result", {{"status", "outcome_unknown"}}}}).status,
            PrimitiveTerminalStatus::kOutcomeUnknown);
}

TEST(PrimitiveAcknowledgementTest, RejectsMissingOrInconsistentTerminalResults) {
  using detail::ParsePrimitiveAcknowledgement;
  using detail::PrimitiveTerminalStatus;

  EXPECT_EQ(ParsePrimitiveAcknowledgement({{"accepted", true}, {"result", nullptr}}).status,
            PrimitiveTerminalStatus::kInvalid);
  EXPECT_EQ(
      ParsePrimitiveAcknowledgement({{"accepted", true}, {"result", {{"status", "rejected"}}}})
          .status,
      PrimitiveTerminalStatus::kInvalid);
  EXPECT_EQ(
      ParsePrimitiveAcknowledgement({{"accepted", false}, {"result", {{"status", "succeeded"}}}})
          .status,
      PrimitiveTerminalStatus::kInvalid);
  EXPECT_EQ(
      ParsePrimitiveAcknowledgement({{"accepted", false}, {"result", {{"status", "timed_out"}}}})
          .status,
      PrimitiveTerminalStatus::kInvalid);
}

TEST_F(SkillRuntimeTest, LoadsDiscoversAndFindsEmbeddedCodeSkills) {
  Runtime runtime = LoadFixture();
  const std::vector<Metadata> skills = runtime.Discover();
  ASSERT_EQ(skills.size(), 12u);
  EXPECT_EQ(skills[0].name, "fixture.abandon_async");
  EXPECT_EQ(skills[1].name, "fixture.async");
  EXPECT_EQ(skills[2].name, "fixture.blocking");
  EXPECT_EQ(skills[3].name, "fixture.cancel");
  EXPECT_EQ(skills[4].name, "fixture.concurrent_async");
  EXPECT_EQ(skills[5].name, "fixture.invalid");
  EXPECT_EQ(skills[6].name, "fixture.invalid_result");
  EXPECT_EQ(skills[7].name, "fixture.pure");
  EXPECT_TRUE(skills[7].primitives.empty());
  EXPECT_EQ(skills[8].name, "fixture.reported_failure");
  EXPECT_EQ(skills[9].name, "fixture.sequence");
  EXPECT_EQ(skills[10].name, "fixture.signal_failure");
  EXPECT_EQ(skills[11].name, "fixture.uncooperative");
  const Metadata* sequence = runtime.Find("fixture.sequence", "1.0.0");
  ASSERT_NE(sequence, nullptr);
  EXPECT_EQ(sequence->library, "fixture");
  EXPECT_EQ(sequence->primitives, std::vector<PrimitiveReference>({{"base.posture", "1.0.0"}}));
  EXPECT_EQ(sequence->parameters_schema.at("type"), "object");
  EXPECT_TRUE(sequence->resources.empty());
  EXPECT_EQ(runtime.Find("fixture.sequence", "9.0.0"), nullptr);
}

TEST_F(SkillRuntimeTest, LoadsAbiV2MetadataAndBridgesSignalHooks) {
  Runtime runtime = Runtime::LoadDirectory(EOS_TEST_SKILL_V2_DIR, dispatcher_);
  const Metadata* metadata = runtime.Find("fixture.signals", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  EXPECT_TRUE(metadata->supports_signals);
  EXPECT_TRUE(metadata->reentrant);
  ASSERT_EQ(metadata->accepted_signals.size(), 1u);
  EXPECT_EQ(metadata->accepted_signals.front().name, "control.update");
  ASSERT_EQ(metadata->emitted_signals.size(), 1u);
  EXPECT_EQ(metadata->emitted_signals.front().name, "progress.changed");
  ASSERT_EQ(metadata->claim_templates.size(), 1u);
  EXPECT_EQ(metadata->claim_templates.front().argument_pointer, "/resource_id");

  std::vector<Signal> emitted;
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds timeout) -> std::optional<Signal> {
    EXPECT_EQ(timeout, std::chrono::milliseconds(0));
    return Signal{"control.update", {{"speed", 2}}};
  };
  hooks.emit_signal = [&](const Signal& signal) {
    emitted.push_back(signal);
    return true;
  };
  const RunResult result = runtime.ExecuteWithHooks(
      "fixture.signals", "1.0.0", {{"resource_id", "arm-0"}}, "v2-command", std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  ASSERT_EQ(emitted.size(), 1u);
  EXPECT_EQ(emitted.front().name, "progress.changed");
  EXPECT_EQ(emitted.front().payload, nlohmann::json({{"speed", 2}}));
}

TEST_F(SkillRuntimeTest, LoadsTwoPackedOldLayoutAbiV2Descriptors) {
  Runtime runtime = Runtime::LoadDirectory(EOS_OLD_LAYOUT_SKILL_V2_DIR, dispatcher_);
  ASSERT_NE(runtime.Find("fixture.old_first", "1.0.0"), nullptr);
  ASSERT_NE(runtime.Find("fixture.old_second", "1.0.0"), nullptr);
  EXPECT_EQ(runtime.Execute("fixture.old_second", "1.0.0", nlohmann::json::object(),
                            "old-layout-second").status,
            RunResult::Status::kCompleted);
}

TEST_F(SkillRuntimeTest, RejectsReceivedSignalsOutsideDeclaredSchema) {
  Runtime runtime = Runtime::LoadDirectory(EOS_TEST_SKILL_V2_DIR, dispatcher_);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return Signal{"control.update", {{"speed", "fast"}}};
  };
  const RunResult result = runtime.ExecuteWithHooks(
      "fixture.signals", "1.0.0", {{"resource_id", "arm-0"}}, "invalid-signal",
      std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "signal.internal");
  EXPECT_TRUE(result.primitive_acks.empty());
}

TEST_F(SkillRuntimeTest, EnforcesSourceSchemaAndInitializesResponses) {
  Runtime runtime = Runtime::LoadDirectory(EOS_TEST_SKILL_V2_DIR, dispatcher_);
  const RunResult unavailable = runtime.Execute("fixture.source_unavailable", "1.0.0",
                                                nlohmann::json::object(), "source-unavailable");
  EXPECT_EQ(unavailable.status, RunResult::Status::kCompleted);

  ExecutionHooks unauthorized_hooks;
  unauthorized_hooks.publish_source = [](const std::string&, std::int64_t, nlohmann::json) {
    ADD_FAILURE() << "unauthorized source write reached provider";
    return SourceSample{};
  };
  EXPECT_EQ(runtime.ExecuteWithHooks("fixture.source_unauthorized", "1.0.0",
                                     nlohmann::json::object(), "source-unauthorized",
                                     std::move(unauthorized_hooks)).status,
            RunResult::Status::kCompleted);

  ExecutionHooks valid_hooks;
  valid_hooks.query_source = [](const std::string& topic, SourceLookup, std::int64_t) {
    return std::optional<SourceSample>(SourceSample{topic, 10, 4, {{"speed", 3}}});
  };
  EXPECT_EQ(runtime.ExecuteWithHooks("fixture.source_read", "1.0.0", nlohmann::json::object(),
                                     "source-valid", std::move(valid_hooks)).status,
            RunResult::Status::kCompleted);

  ExecutionHooks invalid_hooks;
  invalid_hooks.query_source = [](const std::string& topic, SourceLookup, std::int64_t) {
    return std::optional<SourceSample>(SourceSample{topic, 10, 4, {{"speed", "fast"}}});
  };
  EXPECT_EQ(runtime.ExecuteWithHooks("fixture.source_read", "1.0.0", nlohmann::json::object(),
                                     "source-invalid", std::move(invalid_hooks)).status,
            RunResult::Status::kFailed);
}

TEST_F(SkillRuntimeTest, EnforcesAndRunsDeclaredExactFrameInference) {
  std::size_t calls = 0;
  Runtime runtime = Runtime::LoadDirectory(
      EOS_TEST_SKILL_V2_DIR, dispatcher_, {}, {},
       [&](const InferenceRequest& request, Cancelled) {
         ++calls;
         if (request.authority == InferenceAuthority::kVad) {
           EXPECT_TRUE(request.source.empty());
           EXPECT_EQ(request.source_sequence, 0u);
           EXPECT_EQ(request.source_generation, 0u);
           EXPECT_TRUE(request.options.contains("input"));
           return InferenceResult{request.authority, {}, 0, 0, {{"speech", true}}};
         }
         EXPECT_EQ(request.authority, InferenceAuthority::kHumanPose);
         if (request.source.empty()) {
           EXPECT_EQ(request.source_sequence, 0u);
           EXPECT_EQ(request.source_generation, 0u);
           EXPECT_TRUE(request.options.contains("input"));
         } else {
           EXPECT_EQ(request.source, "camera.front");
           EXPECT_EQ(request.source_sequence, 12u);
           EXPECT_EQ(request.source_generation, 9u);
         }
         return InferenceResult{request.authority, request.source, request.source_sequence,
                                request.source_generation, {{"pose", "standing"}}};
       });
  const Metadata* metadata = runtime.Find("fixture.inference", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  EXPECT_EQ(metadata->inference_authorities,
            std::vector<InferenceAuthority>(
                {InferenceAuthority::kVad, InferenceAuthority::kHumanPose}));
  const auto result = runtime.Execute("fixture.inference", "1.0.0", nlohmann::json::object(),
                                      "inference-command");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code << ": "
                                                          << result.message;
  EXPECT_EQ(calls, 3u);
  if (result.status == RunResult::Status::kCompleted)
    EXPECT_EQ(result.result.at("pose"), "standing");
  const auto undeclared = runtime.Execute("fixture.inference_undeclared", "1.0.0",
                                           nlohmann::json::object(), "undeclared-inference");
  EXPECT_EQ(undeclared.status, RunResult::Status::kCompleted);
  EXPECT_EQ(calls, 3u);
}

TEST_F(SkillRuntimeTest, CancelsDispatcherCommandWhileWaitingForAsyncPrimitive) {
  Runtime runtime = Runtime::LoadDirectory(EOS_TEST_SKILL_V2_DIR, dispatcher_);
  std::atomic<bool> cancelled{false};
  std::thread cancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cancelled.store(true);
  });
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return cancelled.load(); };
  hooks.acquire_device = [&](const std::string&) {
    while (!cancelled.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return std::function<void()>{};
  };
  const auto started = std::chrono::steady_clock::now();
  const RunResult result = runtime.ExecuteWithHooks("fixture.async_cancellation", "1.0.0",
                                                    nlohmann::json::object(), "async-cancel",
                                                    std::move(hooks));
  cancel.join();
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
}

TEST_F(SkillRuntimeTest, ValidatesArgumentsBeforeEnteringSkillCode) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.invalid", "1.0.0", {{"unexpected", true}}, "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kInvalidArgument);
  EXPECT_EQ(result.reason_code, "arguments.invalid");
  EXPECT_NE(result.message.find("parameters schema"), std::string::npos);
}

TEST_F(SkillRuntimeTest, ValidatesOneSkillDuringUnrelatedNonReentrantExecution) {
  std::mutex mutex;
  std::condition_variable changed;
  bool execution_entered = false;
  bool release_execution = false;
  Runtime runtime =
      Runtime::LoadDirectory(EOS_TEST_SKILL_DIR, dispatcher_, [&](const std::string&) {
        std::unique_lock<std::mutex> lock(mutex);
        execution_entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release_execution; });
        return nlohmann::json::object();
      });
  std::thread execution([&] {
    runtime.Execute("fixture.blocking", "1.0.0", nlohmann::json::object(), "blocking-command");
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(mutex);
    entered = changed.wait_for(lock, std::chrono::seconds(1), [&] { return execution_entered; });
    if (!entered) release_execution = true;
  }
  if (!entered) {
    changed.notify_all();
    execution.join();
    FAIL() << "blocking skill did not enter its resource provider";
  }

  bool validation_finished = false;
  std::thread validation([&] {
    runtime.ValidateArguments("fixture.pure", "1.0.0", nlohmann::json::object());
    std::lock_guard<std::mutex> lock(mutex);
    validation_finished = true;
    changed.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    EXPECT_TRUE(
        changed.wait_for(lock, std::chrono::seconds(1), [&] { return validation_finished; }));
    release_execution = true;
  }
  changed.notify_all();
  validation.join();
  execution.join();
}

TEST_F(SkillRuntimeTest, ExecutesPureSkillWithoutPrimitiveCapabilities) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.pure", "1.0.0", nlohmann::json::object(), "pure-command");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.primitive_acks.empty());
}

TEST_F(SkillRuntimeTest, RunsCodeThroughDispatcherAndRecordsAcknowledgements) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.sequence", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.reason_code, "completed");
  EXPECT_EQ(result.result, nlohmann::json({{"fixture", true}}));
  ASSERT_EQ(result.primitive_acks.size(), 2u);
  EXPECT_EQ(result.primitive_acks[0].at("command_id"), "skill-command/0");
  EXPECT_EQ(result.primitive_acks[1].at("command_id"), "skill-command/1");
  EXPECT_TRUE(result.primitive_acks[0].at("accepted").get<bool>());
  EXPECT_EQ(result.primitive_acks[0].at("result").at("status"), "succeeded");
  EXPECT_EQ(result.primitive_acks[1].at("result").at("status"), "succeeded");
}

TEST_F(SkillRuntimeTest, ExecutesAsynchronousPrimitive) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.async", "1.0.0", nlohmann::json::object(), "async-skill-command");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  ASSERT_EQ(result.primitive_acks.size(), 1u);
  EXPECT_EQ(result.primitive_acks.front().at("command_id"), "async-skill-command/0");
  EXPECT_EQ(result.primitive_acks.front().at("result").at("status"), "succeeded");
}

TEST_F(SkillRuntimeTest, ControlsConcurrentAsynchronousPrimitivesIndependently) {
  Runtime runtime = LoadFixture();
  const RunResult result = runtime.Execute("fixture.concurrent_async", "1.0.0",
                                           nlohmann::json::object(), "concurrent-command");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  ASSERT_EQ(result.primitive_acks.size(), 2u);
  EXPECT_EQ(result.primitive_acks[0].at("command_id"), "concurrent-command/1");
  EXPECT_EQ(result.primitive_acks[1].at("command_id"), "concurrent-command/0");
}

TEST_F(SkillRuntimeTest, DrainsEveryAbandonedAsynchronousPrimitiveAndTearsDownOnSkillThread) {
  Runtime runtime = LoadFixture();
  const std::thread::id skill_thread = std::this_thread::get_id();
  std::atomic<std::size_t> acquired{0};
  std::size_t released = 0;
  std::size_t begun = 0;
  std::size_t ended = 0;
  ExecutionHooks hooks;
  hooks.acquire_device = [&](const std::string&) {
    ++acquired;
    return [&] {
      EXPECT_EQ(std::this_thread::get_id(), skill_thread);
      ++released;
    };
  };
  hooks.begin_activity = [&](const Activity&) {
    EXPECT_EQ(std::this_thread::get_id(), skill_thread);
    return "activity-" + std::to_string(++begun);
  };
  hooks.end_activity = [&](const std::string&, const std::string&) {
    EXPECT_EQ(std::this_thread::get_id(), skill_thread);
    ++ended;
  };

  const RunResult result = runtime.ExecuteWithHooks("fixture.abandon_async", "1.0.0",
                                                     nlohmann::json::object(), "abandon-command",
                                                     std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kProtocolError);
  EXPECT_EQ(acquired.load(), 2u);
  EXPECT_EQ(released, 2u);
  EXPECT_EQ(begun, 2u);
  EXPECT_EQ(ended, 2u);
}



TEST_F(SkillRuntimeTest, LatchesHostAfterRejectedPrimitive) {
  Runtime runtime = LoadFixture();
  const_cast<hardware::Backend&>(registry_.FindDevice("base-0")->feeds()).Halt();
  const RunResult result =
      runtime.Execute("fixture.sequence", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code, "primitive.rejected");
  ASSERT_EQ(result.primitive_acks.size(), 1u);
  EXPECT_FALSE(result.primitive_acks.front().at("accepted").get<bool>());
}

TEST_F(SkillRuntimeTest, MapsSkillArgumentRejection) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.invalid", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kInvalidArgument);
  EXPECT_FALSE(result.accepted);
}

TEST_F(SkillRuntimeTest, PreservesReportedSkillFailure) {
  Runtime runtime = LoadFixture();
  const RunResult result = runtime.Execute("fixture.reported_failure", "1.0.0",
                                           nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "fixture.backend_failed");
  EXPECT_EQ(result.message, "fixture backend was unavailable");
}

TEST_F(SkillRuntimeTest, AcceptsLegacyCompletionSize) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.pure", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.reason_code, "completed");
  EXPECT_TRUE(result.message.empty());
}

TEST_F(SkillRuntimeTest, InfersLegacySignalFailure) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.signal_failure", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "signal.unavailable");
  EXPECT_EQ(result.message, "signal delivery is unavailable");
}

TEST_F(SkillRuntimeTest, RejectsResultsOutsideDeclaredSchema) {
  Runtime runtime = LoadFixture();
  const RunResult result =
      runtime.Execute("fixture.invalid_result", "1.0.0", nlohmann::json::object(), "skill-command");
  EXPECT_EQ(result.status, RunResult::Status::kProtocolError);
  EXPECT_EQ(result.reason_code, "skill.protocol_error");
  EXPECT_NE(result.message.find("returns schema"), std::string::npos);
}

TEST_F(SkillRuntimeTest, PreservesCancellationAcrossNativeAbi) {
  Runtime runtime = LoadFixture();
  const RunResult result = runtime.Execute("fixture.cancel", "1.0.0", nlohmann::json::object(),
                                           "skill-command", [] { return true; });
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(result.reason_code, "cancelled");
  EXPECT_TRUE(result.primitive_acks.empty());
}

TEST_F(SkillRuntimeTest, CancellationLatchesPrimitiveReleaseForUncooperativeSkill) {
  Runtime runtime = LoadFixture();
  const RunResult result = runtime.Execute("fixture.sequence", "1.0.0", nlohmann::json::object(),
                                           "skill-command", [] { return true; });
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(result.reason_code, "cancelled");
  EXPECT_TRUE(result.primitive_acks.empty());
}

TEST_F(SkillRuntimeTest, RejectsRelativeAndMissingDirectories) {
  EXPECT_THROW(Runtime::LoadDirectory("relative", dispatcher_), std::runtime_error);
  EXPECT_THROW(Runtime::LoadDirectory("/definitely/missing/eos-skills", dispatcher_),
               std::runtime_error);
}

TEST_F(SkillRuntimeTest, RejectsIncompatibleAbiAndUnknownPrimitive) {
  EXPECT_THROW(Runtime::LoadDirectory(EOS_BAD_ABI_SKILL_DIR, dispatcher_), std::runtime_error);
  EXPECT_THROW(Runtime::LoadDirectory(EOS_BAD_PRIMITIVE_SKILL_DIR, dispatcher_),
               std::runtime_error);
}

TEST_F(SkillRuntimeTest, RejectsMisalignedDescriptorStride) {
  EXPECT_THROW(Runtime::LoadDirectory(EOS_BAD_DESCRIPTOR_STRIDE_SKILL_DIR, dispatcher_),
               std::runtime_error);
}

TEST_F(SkillRuntimeTest, RejectsDescriptorLargerThanStride) {
  EXPECT_THROW(Runtime::LoadDirectory(EOS_BAD_DESCRIPTOR_SIZE_SKILL_DIR, dispatcher_),
               std::runtime_error);
}

TEST_F(SkillRuntimeTest, RejectsLegacyV1OnlyLibrary) {
  try {
    (void)Runtime::LoadDirectory(EOS_LEGACY_V1_SKILL_DIR, dispatcher_);
    FAIL() << "legacy ABI v1 library was admitted";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("has no ABI v2 query symbol"), std::string::npos);
  }
}

TEST_F(SkillRuntimeTest, TracksAsynchronousExecutionStateAndEvents) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  const std::string id =
      executions.Start("fixture.sequence", "1.0.0", nlohmann::json::object(), "async-command");
  const auto events = executions.WaitForEvents(id, 1, std::chrono::seconds(1));
  ASSERT_FALSE(events.empty());

  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto snapshot = executions.Get(id);
    ASSERT_TRUE(snapshot.has_value());
    if (IsTerminal(snapshot->status)) {
      EXPECT_EQ(snapshot->status, ExecutionStatus::kSucceeded);
      EXPECT_EQ(snapshot->reason_code, "completed");
      EXPECT_EQ(snapshot->primitive_acks.size(), 2u);
      EXPECT_GE(snapshot->latest_journal_sequence, 3u);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "asynchronous skill did not finish";
}

TEST_F(SkillRuntimeTest, RejectsInvalidArgumentsBeforeQueueingExecution) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  EXPECT_THROW(
      executions.Start("fixture.sequence", "1.0.0", {{"unexpected", true}}, "invalid-arguments"),
      std::invalid_argument);
}

TEST_F(SkillRuntimeTest, DeduplicatesConcurrentStartsAndRejectsConflictingRequestReuse) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  constexpr std::size_t kCallers = 8;
  std::array<std::string, kCallers> ids;
  std::vector<std::thread> callers;
  for (std::size_t index = 0; index < kCallers; ++index) {
    callers.emplace_back([&, index] {
      const StartResult started =
          executions.Start("fixture.sequence", "1.0.0", nlohmann::json::object(),
                           "deduplicated-request", StartOptions{std::chrono::seconds(10)});
      ids[index] = started.execution.execution_id;
    });
  }
  for (auto& caller : callers) caller.join();
  ASSERT_FALSE(ids.front().empty());
  for (const auto& id : ids) EXPECT_EQ(id, ids.front());

  const StartResult conflict =
      executions.Start("fixture.pure", "1.0.0", nlohmann::json::object(), "deduplicated-request",
                       StartOptions{std::chrono::seconds(10)});
  EXPECT_EQ(conflict.status, StartResult::Status::kConflict);
  EXPECT_EQ(conflict.execution.execution_id, ids.front());
}


TEST_F(SkillRuntimeTest, CanonicalizesCompletionAfterCancellation) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  const std::string id = executions.Start("fixture.uncooperative", "1.0.0",
                                          nlohmann::json::object(), "cancel-uncooperative");
  const auto running = executions.WaitForEvents(id, 1, std::chrono::seconds(1));
  ASSERT_FALSE(running.empty());
  ASSERT_TRUE(executions.Terminate(id));

  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto snapshot = executions.Get(id);
    ASSERT_TRUE(snapshot.has_value());
    if (IsTerminal(snapshot->status)) {
      EXPECT_EQ(snapshot->status, ExecutionStatus::kCancelled);
      EXPECT_EQ(snapshot->reason_code, "cancelled");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  FAIL() << "uncooperative skill did not finish";
}

TEST_F(SkillRuntimeTest, PropagatesRequestCorrelationToWorkerExecution) {
  std::atomic<bool> saw_correlation{false};
  Runtime runtime = Runtime::LoadDirectory(
      EOS_TEST_SKILL_DIR, dispatcher_, [&saw_correlation](const std::string&) {
        saw_correlation.store(eos::lib::CurrentCorrelationId() == "async-skill-request");
        return nlohmann::json::object();
      });
  ExecutionService executions(runtime);
  std::string id;
  {
    eos::lib::CorrelationScope correlation("async-skill-request");
    id = executions.Start("fixture.blocking", "1.0.0", nlohmann::json::object(),
                          "correlated-command");
  }
  for (int attempt = 0; attempt < 200 && !saw_correlation.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  executions.Terminate(id);
  bool terminal = false;
  for (int attempt = 0; attempt < 2000; ++attempt) {
    const auto snapshot = executions.Get(id);
    ASSERT_TRUE(snapshot.has_value());
    if (IsTerminal(snapshot->status)) {
      terminal = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(terminal);
  EXPECT_TRUE(saw_correlation.load());
}

TEST_F(SkillRuntimeTest, EvictsOldestTerminalRecordsAtRetentionLimit) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  std::string first_id;
  std::string latest_id;
  const auto wait_until_terminal = [&executions](const std::string& id) {
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto snapshot = executions.Get(id);
      if (!snapshot || IsTerminal(snapshot->status)) return snapshot;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return executions.Get(id);
  };
  for (std::size_t execution = 0; execution < 1024; ++execution) {
    latest_id = executions.Start("fixture.pure", "1.0.0", nlohmann::json::object(),
                                 "retention-" + std::to_string(execution));
    if (execution == 0) first_id = latest_id;
    const auto snapshot = wait_until_terminal(latest_id);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(IsTerminal(snapshot->status));
  }

  const auto first = executions.Get(first_id);
  ASSERT_TRUE(first.has_value());
  std::atomic<bool> watcher_started{false};
  std::thread watcher([&] {
    watcher_started.store(true);
    executions.WaitForEvents(first_id, first->latest_journal_sequence,
                             std::chrono::milliseconds(100));
  });
  while (!watcher_started.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  latest_id =
      executions.Start("fixture.pure", "1.0.0", nlohmann::json::object(), "retention-with-watcher");
  const auto latest = wait_until_terminal(latest_id);
  EXPECT_TRUE(latest.has_value());
  EXPECT_TRUE(executions.Get(first_id).has_value());
  watcher.join();

  latest_id = executions.Start("fixture.pure", "1.0.0", nlohmann::json::object(),
                               "retention-after-watcher");
  const auto retained = wait_until_terminal(latest_id);
  ASSERT_TRUE(retained.has_value());
  EXPECT_FALSE(executions.Get(first_id).has_value());
  EXPECT_EQ(retained->status, ExecutionStatus::kSucceeded);
}


TEST_F(SkillRuntimeTest, EmitsBoundedStructuredLifecycleEventsWithoutPollingNoise) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  auto sink = std::make_shared<SkillLogSink>();
  SkillLogger()->sinks().push_back(sink);

  const std::string id =
      executions.Start("fixture.sequence", "1.0.0", nlohmann::json::object(), "logged-command");
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto snapshot = executions.Get(id);
    ASSERT_TRUE(snapshot.has_value());
    if (IsTerminal(snapshot->status) && sink->lifecycle_count.load() >= 5) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SkillLogger()->sinks().erase(
      std::find(SkillLogger()->sinks().begin(), SkillLogger()->sinks().end(), sink));

  std::vector<std::string> lifecycle;
  for (const std::string& message : sink->messages)
    if (message.find("[skill_execution_") != std::string::npos) lifecycle.push_back(message);
  ASSERT_EQ(lifecycle.size(), 5u);
  const auto contains = [&](const std::string& event) {
    return std::find_if(lifecycle.begin(), lifecycle.end(), [&](const std::string& message) {
             return message.find(event) != std::string::npos;
           }) != lifecycle.end();
  };
  EXPECT_TRUE(contains("skill_execution_routed"));
  EXPECT_TRUE(contains("skill_execution_queued"));
  EXPECT_TRUE(contains("skill_execution_started"));
  EXPECT_TRUE(contains("skill_execution_terminal"));
  EXPECT_TRUE(contains("skill_execution_accounting"));
  EXPECT_TRUE(contains("execution_id=" + id));
  for (const std::string& message : lifecycle) {
    EXPECT_EQ(message.find("arguments="), std::string::npos);
    EXPECT_EQ(message.find("result="), std::string::npos);
    EXPECT_EQ(message.find("primitive_acks="), std::string::npos);
  }
}

TEST_F(SkillRuntimeTest, EmitsActionableTerminalFailureWithoutPayloads) {
  Runtime runtime = LoadFixture();
  ExecutionService executions(runtime);
  auto sink = std::make_shared<SkillLogSink>();
  SkillLogger()->sinks().push_back(sink);

  const std::string id = executions.Start("fixture.reported_failure", "1.0.0",
                                          nlohmann::json::object(), "failure-command");
  // The terminal state is published before its log record is emitted, so wait
  // for both: stopping at the state alone would race the sink update.
  for (int attempt = 0; attempt < 300; ++attempt) {
    const auto snapshot = executions.Get(id);
    ASSERT_TRUE(snapshot.has_value());
    if (IsTerminal(snapshot->status) &&
        sink->HasLineStartingWith("skill_execution_terminal")) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  SkillLogger()->sinks().erase(
      std::find(SkillLogger()->sinks().begin(), SkillLogger()->sinks().end(), sink));

  const auto snapshot = executions.Get(id);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->reason_code, "fixture.backend_failed");
  EXPECT_EQ(snapshot->message, "fixture backend was unavailable");
  const auto terminal = std::find_if(
      sink->messages.begin(), sink->messages.end(),
      [](const std::string& message) {
        return message.find("[skill_execution_terminal]") != std::string::npos;
      });
  ASSERT_NE(terminal, sink->messages.end());
  EXPECT_NE(terminal->find(": fixture.backend_failed"), std::string::npos);
  EXPECT_NE(terminal->find("message=fixture backend was unavailable"), std::string::npos);
  EXPECT_NE(terminal->find("primitive_calls=0"), std::string::npos);
  EXPECT_EQ(terminal->find("arguments="), std::string::npos);
  EXPECT_EQ(terminal->find("result="), std::string::npos);
  EXPECT_EQ(terminal->find("primitive_acks="), std::string::npos);
}

TEST_F(SkillRuntimeTest, RejectedInvocationDoesNotClaimExecutionStartedOrLogPayload) {
  Runtime runtime = LoadFixture();
  auto sink = std::make_shared<SkillLogSink>();
  SkillLogger()->sinks().push_back(sink);

  const RunResult result =
      runtime.Execute("fixture.sequence", "1.0.0", {{"private", "do-not-log"}}, "");

  SkillLogger()->sinks().erase(
      std::find(SkillLogger()->sinks().begin(), SkillLogger()->sinks().end(), sink));
  EXPECT_EQ(result.status, RunResult::Status::kInvalidArgument);
  ASSERT_EQ(sink->messages.size(), 1u);
  EXPECT_NE(sink->messages.front().find("skill_execute_rejected"), std::string::npos);
  EXPECT_EQ(sink->messages.front().find("skill_execute_started"), std::string::npos);
  EXPECT_EQ(sink->messages.front().find("skill_execute_completed"), std::string::npos);
  EXPECT_EQ(sink->messages.front().find("do-not-log"), std::string::npos);
}
}  // namespace
}  // namespace eos::runtime::skills::native

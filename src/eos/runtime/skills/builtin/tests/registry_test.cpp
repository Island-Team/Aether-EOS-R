#include "src/eos/runtime/skills/builtin/registry.h"
#include "share/skills/native/skills.h"
#include "src/eos/lib/timer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace eos::runtime::skills::builtin {
namespace {

class BuiltinRegistryTest : public testing::Test {
 protected:
  eos::lib::ManualTimerService timer_;
};

TEST_F(BuiltinRegistryTest, RegistersAllNativeProductSkills) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);
  EXPECT_NE(registry.Find("observe_human", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("interact_waving_owner", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("interact_fallen_owner", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("interact_furniture_owner", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("register_host", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("head_find_trash", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("pick_trash", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("navigate_waypoint", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("navigate_pick_resume", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("navigate_announce_trash", "1.0.0"), nullptr);
  EXPECT_NE(registry.Find("navigate_pick_trash", "1.0.0"), nullptr);
}

TEST_F(BuiltinRegistryTest, NativeProductSkillsDeclareRequiredLongLivedActuatorClaims) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);
  const auto expect_claims = [&](const char* name, const char* version,
                                 std::vector<std::string> pointers) {
    const Metadata* metadata = registry.Find(name, version);
    ASSERT_NE(metadata, nullptr);
    ASSERT_EQ(metadata->claim_templates.size(), pointers.size()) << name;
    for (std::size_t index = 0; index < pointers.size(); ++index) {
      EXPECT_EQ(metadata->claim_templates[index].kind, ClaimKind::kDevice) << name;
      EXPECT_EQ(metadata->claim_templates[index].mode, ClaimMode::kExclusiveUse) << name;
      EXPECT_EQ(metadata->claim_templates[index].argument_pointer, pointers[index]) << name;
    }
  };

  expect_claims("observe_human", "1.0.0", {"/gimbal_device_id"});
  expect_claims("interact_waving_owner", "1.0.0",
                {"/base_device_id", "/speaker_device_id"});
  expect_claims("interact_fallen_owner", "1.0.0",
                {"/base_device_id", "/speaker_device_id", "/arm_device_id"});
  expect_claims("interact_furniture_owner", "1.0.0",
                 {"/base_device_id", "/speaker_device_id", "/arm_device_id"});
  expect_claims("register_host", "1.0.0", {"/camera_device_id", "/speaker_device_id"});
  expect_claims("head_find_trash", "1.0.0", {});
  expect_claims("pick_trash", "1.0.0", {"/arm_device_id", "/speaker_device_id"});
  expect_claims("navigate_waypoint", "1.0.0", {"/base_device_id"});
  expect_claims("navigate_pick_resume", "1.0.0",
                 {"/base_device_id", "/speaker_device_id", "/arm_device_id",
                  "/gripper_device_id"});
  expect_claims("navigate_announce_trash", "1.0.0",
                {"/base_device_id", "/speaker_device_id"});
  expect_claims("navigate_pick_trash", "1.0.0",
                {"/base_device_id", "/speaker_device_id", "/arm_device_id",
                 "/gripper_device_id"});
}

TEST_F(BuiltinRegistryTest, ArmActuatingOwnerSkillsDeclareArmInit) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);

  for (const char* name : {"interact_fallen_owner", "interact_furniture_owner"}) {
    const Metadata* metadata = registry.Find(name, "1.0.0");
    ASSERT_NE(metadata, nullptr);
    EXPECT_NE(std::find(metadata->primitives.begin(), metadata->primitives.end(),
                        PrimitiveReference{"arm.init", "1.0.0"}),
              metadata->primitives.end())
        << name;
  }
}

TEST_F(BuiltinRegistryTest, NavigateTrashSkillsDeclareExclusiveTargetInputs) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);

  for (const char* name : {"navigate_announce_trash", "navigate_pick_trash"}) {
    const Metadata* metadata = registry.Find(name, "1.0.0");
    ASSERT_NE(metadata, nullptr);
    ASSERT_TRUE(metadata->parameters_schema.contains("oneOf"));
    EXPECT_EQ(metadata->parameters_schema.at("oneOf").size(), 2u);
    const auto& properties = metadata->parameters_schema.at("properties");
    EXPECT_TRUE(properties.contains("start_waypoint_id"));
    EXPECT_TRUE(properties.contains("start_relative_target"));
  }

  const Metadata* announce = registry.Find("navigate_announce_trash", "1.0.0");
  ASSERT_NE(announce, nullptr);
  EXPECT_FALSE(announce->parameters_schema.at("properties").contains("arm_device_id"));

  const Metadata* pick = registry.Find("navigate_pick_trash", "1.0.0");
  ASSERT_NE(pick, nullptr);
  EXPECT_TRUE(pick->parameters_schema.at("properties").contains("arm_device_id"));
  EXPECT_TRUE(pick->parameters_schema.at("properties").contains("gripper_device_id"));
}

TEST_F(BuiltinRegistryTest, RegisterHostDeclaresAsrInput) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);
  const Metadata* metadata = registry.Find("register_host", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  EXPECT_EQ(metadata->parameters_schema.at("required"),
            nlohmann::json({"robot_name", "camera_device_id", "speaker_device_id"}));
  const auto& timeout = metadata->parameters_schema.at("properties").at("introduction_timeout_ms");
  EXPECT_EQ(timeout.at("minimum"), 1000);
  EXPECT_EQ(timeout.at("maximum"), 300000);
  EXPECT_EQ(timeout.at("default"), 60000);
  ASSERT_EQ(metadata->resources.size(), 1u);
  EXPECT_EQ(metadata->resources.front().uri_template,
            "robots/{robot_name}/world/asr/state");
}

TEST_F(BuiltinRegistryTest, OwnerInteractionUsesWorldResourcesWithoutInferenceAuthority) {
  Registry registry(timer_);
  island::skills::native::RegisterSkills(registry);
  ASSERT_NE(registry.Find("interact_waving_owner", "1.0.0"), nullptr);
  EXPECT_TRUE(registry.Find("interact_waving_owner", "1.0.0")
                  ->inference_authorities.empty());
  EXPECT_TRUE(registry.Find("interact_fallen_owner", "1.0.0")
                  ->inference_authorities.empty());
  EXPECT_TRUE(registry.Find("interact_furniture_owner", "1.0.0")
                  ->inference_authorities.empty());
}

TEST_F(BuiltinRegistryTest, NavigateWaypointResolvesResourceAndCallsPrimitive) {
  std::string resource_uri;
  nlohmann::json primitive_arguments;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    resource_uri = uri;
    return nlohmann::json{{"reference", "absolute"}, {"frame_id", "map"},
                          {"x_m", 4.2}, {"y_m", -1.1}, {"yaw_rad", 1.57}};
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string& device_id,
          nlohmann::json arguments, const std::string&, Cancelled) {
        EXPECT_EQ(primitive.name, "base.navigate");
        EXPECT_EQ(device_id, "base-0");
        primitive_arguments = std::move(arguments);
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);

  const RunResult result = registry.Execute(
      "navigate_waypoint", "1.0.0",
      {{"robot_name", "demo-0"}, {"base_device_id", "base-0"},
       {"waypoint_id", "room-1"}},
      "navigate-test");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(resource_uri, "robots/demo-0/navigation/waypoints/room-1");
  EXPECT_EQ(primitive_arguments.at("frame_id"), "map");
  EXPECT_EQ(result.result.at("waypoint_id"), "room-1");
}

TEST_F(BuiltinRegistryTest, NavigateWaypointUsesInlineTargetWithoutResourceRead) {
  bool resource_read = false;
  nlohmann::json primitive_arguments;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string&) {
    resource_read = true;
    return nlohmann::json::object();
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference&, const std::string&, nlohmann::json arguments,
          const std::string&, Cancelled) {
        primitive_arguments = std::move(arguments);
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  const nlohmann::json target{{"reference", "relative"}, {"x_m", 1.0},
                              {"y_m", 0.0}, {"yaw_rad", 0.0}};

  const RunResult result = registry.Execute(
      "navigate_waypoint", "1.0.0",
      {{"robot_name", "demo-0"}, {"base_device_id", "base-0"},
       {"waypoint_id", "room-1"}, {"target", target}},
      "navigate-inline-test");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(resource_read);
  EXPECT_EQ(primitive_arguments, target);
}

TEST_F(BuiltinRegistryTest, NavigateWaypointRejectionReturnsDeclaredContext) {
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive =
      [](const PrimitiveReference&, const std::string&, nlohmann::json, const std::string&,
         Cancelled) {
        return nlohmann::json{{"accepted", false}, {"result", {{"status", "rejected"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  const nlohmann::json target{{"reference", "relative"}, {"x_m", 1.0},
                              {"y_m", 0.0}, {"yaw_rad", 0.0}};

  const RunResult result = registry.Execute(
      "navigate_waypoint", "1.0.0",
      {{"robot_name", "demo-0"}, {"base_device_id", "base-0"},
       {"waypoint_id", "room-1"}, {"target", target}},
      "navigate-rejected-test");

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.result, nlohmann::json({{"waypoint_id", "room-1"}, {"target", target}}));
}

Definition EchoDefinition(bool reentrant = true) {
  Definition definition;
  definition.metadata.name = "test.echo";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Echoes one string";
  definition.metadata.parameters_schema = {
      {"type", "object"},
      {"properties", {{"value", {{"type", "string"}}}}},
      {"required", {"value"}},
      {"additionalProperties", false}};
  definition.metadata.returns_schema = {
      {"type", "object"},
      {"properties", {{"echo", {{"type", "string"}}}}},
      {"required", {"echo"}},
      {"additionalProperties", false}};
  definition.metadata.reentrant = reentrant;
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    co_return Outcome::Completed({{"echo", execution.arguments().at("value")}});
  };
  return definition;
}

TEST_F(BuiltinRegistryTest, RegistersDiscoversAndExecutesCoroutine) {
  Registry registry(timer_);
  registry.Register(EchoDefinition());
  ASSERT_EQ(registry.Discover().size(), 1u);
  ASSERT_NE(registry.Find("test.echo", "1.0.0"), nullptr);

  const RunResult result =
      registry.Execute("test.echo", "1.0.0", {{"value", "hello"}}, "command-1");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.result, nlohmann::json({{"echo", "hello"}}));
}

TEST_F(BuiltinRegistryTest, RejectsDuplicateAndInvalidArguments) {
  Registry registry(timer_);
  registry.Register(EchoDefinition());
  EXPECT_THROW(registry.Register(EchoDefinition()), std::invalid_argument);
  EXPECT_THROW(registry.ValidateArguments("test.echo", "1.0.0", {{"value", 7}}),
               std::invalid_argument);

  const RunResult result =
      registry.Execute("test.echo", "1.0.0", {{"value", 7}}, "command-2");
  EXPECT_EQ(result.status, RunResult::Status::kInvalidArgument);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code, "invalid_arguments");
}

TEST_F(BuiltinRegistryTest, RejectsInvalidCompletedResult) {
  Registry registry(timer_);
  Definition definition = EchoDefinition();
  definition.handler = [](Execution&) -> Task<Outcome> {
    co_return Outcome::Completed({{"echo", 42}});
  };
  registry.Register(std::move(definition));
  const RunResult result =
      registry.Execute("test.echo", "1.0.0", {{"value", "hello"}}, "command-3");
  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "invalid_result");
}

struct ResumeOnThread {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> continuation) const {
    std::thread([continuation] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continuation.resume();
    }).detach();
  }
  void await_resume() const noexcept {}
};

TEST_F(BuiltinRegistryTest, CoroutineMayResumeAsynchronously) {
  Registry registry(timer_);
  Definition definition = EchoDefinition();
  definition.handler = [](Execution&) -> Task<Outcome> {
    co_await ResumeOnThread{};
    co_return Outcome::Completed({{"echo", "async"}});
  };
  registry.Register(std::move(definition));
  const RunResult result =
      registry.Execute("test.echo", "1.0.0", {{"value", "unused"}}, "command-4");
  EXPECT_EQ(result.result, nlohmann::json({{"echo", "async"}}));
}

struct SuspendUntilResumed {
  std::coroutine_handle<>* continuation;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> value) const { *continuation = value; }
  void await_resume() const noexcept {}
};

TEST_F(BuiltinRegistryTest, ExecuteAsyncReturnsWhileCoroutineIsSuspended) {
  Registry registry(timer_);
  std::coroutine_handle<> continuation;
  Definition definition = EchoDefinition();
  definition.handler = [&](Execution& execution) -> Task<Outcome> {
    co_await SuspendUntilResumed{&continuation};
    co_return Outcome::Completed({{"echo", execution.arguments().at("value")}});
  };
  registry.Register(std::move(definition));
  std::optional<RunResult> completed;
  registry.ExecuteAsync("test.echo", "1.0.0", {{"value", "resumed"}}, "async-command", {},
                        [&](RunResult result) { completed = std::move(result); });

  ASSERT_TRUE(continuation);
  EXPECT_FALSE(completed.has_value());
  continuation.resume();
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->status, RunResult::Status::kCompleted);
  EXPECT_EQ(completed->result, nlohmann::json({{"echo", "resumed"}}));
}

TEST_F(BuiltinRegistryTest, ExposesOnlyScopedExecutionHooks) {
  Registry registry(timer_);
  Definition definition = EchoDefinition();
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    const auto checkpoint = execution.Checkpoint({"ready", "ready", 0.5, {}});
    if (!checkpoint) co_return Outcome::Failed(checkpoint.error().code);
    co_return execution.cancelled() ? Outcome::Cancelled() : Outcome::Completed({{"echo", "ok"}});
  };
  registry.Register(std::move(definition));
  bool observed = false;
  ExecutionHooks hooks;
  hooks.cancelled = [] { return false; };
  hooks.emit_checkpoint = [&](const Checkpoint& checkpoint) {
    observed = checkpoint.name == "ready" && checkpoint.fields.is_object();
    return true;
  };
  const RunResult result = registry.ExecuteWithHooks(
      "test.echo", "1.0.0", {{"value", "unused"}}, "command-5", std::move(hooks));
  EXPECT_TRUE(observed);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
}

TEST_F(BuiltinRegistryTest, BridgesTypedSignalHooks) {
  Registry registry(timer_);
  Definition definition = EchoDefinition();
  definition.metadata.supports_signals = true;
  definition.metadata.accepted_signals.push_back(
      {"skill.command", {{"type", "object"}}});
  definition.metadata.emitted_signals.push_back(
      {"skill.event", {{"type", "object"}}});
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    auto received = execution.ReceiveSignal(std::chrono::milliseconds(0));
    if (!received || !received.value()) co_return Outcome::Failed("signal.missing");
    auto emitted = execution.EmitSignal("skill.event", received.value()->payload);
    if (!emitted) co_return Outcome::Failed(emitted.error().code);
    co_return Outcome::Completed({{"echo", "signalled"}});
  };
  registry.Register(std::move(definition));

  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return Signal{"skill.command", {{"command", "pause"}}};
  };
  nlohmann::json emitted;
  hooks.emit_signal = [&](const Signal& signal) {
    emitted = signal.payload;
    return signal.name == "skill.event";
  };
  const RunResult result = registry.ExecuteWithHooks(
      "test.echo", "1.0.0", {{"value", "unused"}}, "command-signals", std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(emitted, nlohmann::json({{"command", "pause"}}));
}

TEST_F(BuiltinRegistryTest, ReapsAbandonedPrimitiveOperationsWhenExecutionCompletes) {
  std::mutex cancelled_mutex;
  std::condition_variable cancelled_condition;
  std::unordered_set<std::string> cancelled_commands;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json, const std::string& command_id, Cancelled) {
    std::unique_lock lock(cancelled_mutex);
    cancelled_condition.wait(lock, [&] { return cancelled_commands.contains(command_id); });
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "cancelled"}}}};
  };
  capabilities.cancel_primitive = [&](const std::string& command_id) {
    {
      std::lock_guard lock(cancelled_mutex);
      cancelled_commands.insert(command_id);
    }
    cancelled_condition.notify_all();
  };
  Registry registry(timer_, std::move(capabilities));
  Definition definition;
  definition.metadata.name = "test.abandon";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Cancels a primitive without waiting for its result";
  definition.metadata.parameters_schema =
      {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.returns_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.primitives.push_back({"gimbal.motion", "1.0.0"});
  definition.metadata.reentrant = true;
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    auto operation = execution.StartPrimitive({"gimbal.motion", "1.0.0"}, "gimbal-0",
                                              {{"reference", "relative"}, {"pan_rad", 0.1}});
    if (!operation) co_return Outcome::Failed("start_failed", operation.error().message);
    auto cancelled = execution.CancelPrimitive(operation.value());
    if (!cancelled) co_return Outcome::Failed("cancel_failed", cancelled.error().message);
    co_return Outcome::Completed(nlohmann::json::object());
  };
  registry.Register(std::move(definition));

  for (std::size_t run = 0; run < 300; ++run) {
    const RunResult result =
        registry.Execute("test.abandon", "1.0.0", nlohmann::json::object(),
                         "abandon-" + std::to_string(run));
    ASSERT_EQ(result.status, RunResult::Status::kCompleted) << "run " << run;
    ASSERT_TRUE(result.accepted) << "run " << run;
  }
}

}  // namespace
}  // namespace eos::runtime::skills::builtin

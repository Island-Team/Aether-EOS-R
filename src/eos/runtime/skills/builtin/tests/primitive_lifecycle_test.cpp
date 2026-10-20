#include "share/skills/native/common/primitive_support.h"
#include "share/skills/native/skills.h"
#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace eos::runtime::skills::builtin {
namespace {

using namespace std::chrono_literals;

class PrimitiveLifecycleTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

nlohmann::json WavingOwner(std::uint64_t sequence, std::optional<double> distance_m,
                             std::string name = "Alice", std::string id = "human:1",
                             std::string action = "waving",
                             std::string posture_state = "standing", std::string pose = "stand") {
  nlohmann::json human{{"id", std::move(id)},
                       {"kind", "human"},
                       {"confidence", 0.95},
                       {"bbox", {450, 100, 550, 700}},
                       {"attributes",
                         {{"name", std::move(name)},
                           {"pose", std::move(pose)},
                           {"posture_state", std::move(posture_state)},
                           {"action", std::move(action)}}}};
  if (distance_m) human["distance_m"] = *distance_m;
  return {{"source_sequence", sequence},
          {"image_width_px", 1000},
          {"image_height_px", 800},
          {"detections", nlohmann::json::array({std::move(human)})}};
}

nlohmann::json ApproachOwnerArguments() {
  return {{"robot_name", "demo-0"},
          {"base_device_id", "base-0"},
          {"speaker_device_id", "speaker-0"}};
}

TEST_F(PrimitiveLifecycleTest, ManagerCancellationCancelsAndDrainsBeforeHandlerReturns) {
  std::mutex mutex;
  std::condition_variable changed;
  bool primitive_started = false;
  bool primitive_cancelled = false;
  bool primitive_terminal = false;
  bool device_acquired = false;
  bool device_released = false;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json, const std::string&, Cancelled) {
    std::unique_lock lock(mutex);
    EXPECT_TRUE(device_acquired);
    EXPECT_FALSE(device_released);
    primitive_started = true;
    changed.notify_all();
    changed.wait(lock, [&] { return primitive_cancelled; });
    lock.unlock();
    std::this_thread::sleep_for(20ms);
    lock.lock();
    primitive_terminal = true;
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "cancelled"}}}};
  };
  capabilities.cancel_primitive = [&](const std::string&) {
    std::lock_guard lock(mutex);
    primitive_cancelled = true;
    changed.notify_all();
  };
  Registry registry(timer_, std::move(capabilities));
  Definition definition;
  definition.metadata.name = "test.primitive_lifecycle";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Exercises primitive cancellation lifecycle";
  definition.metadata.parameters_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.returns_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.primitives.push_back({"test.operation", "1.0.0"});
  definition.metadata.reentrant = true;
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    auto task = island::skills::primitive::Run(
        execution, {"test.operation", "1.0.0"}, "device-0", nlohmann::json::object());
    const auto result = co_await task;
    co_return result.status == island::skills::primitive::Status::kCancelled
                  ? Outcome::Cancelled("cancelled", result.message)
                  : Outcome::Failed("unexpected_terminal", result.message);
  };
  registry.Register(std::move(definition));

  std::atomic<bool> manager_cancelled{false};
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return manager_cancelled.load(); };
  hooks.acquire_device = [&](const std::string& device_id) {
    EXPECT_EQ(device_id, "device-0");
    std::lock_guard lock(mutex);
    device_acquired = true;
    return std::function<void()>([&] {
      std::lock_guard release_lock(mutex);
      EXPECT_TRUE(primitive_terminal);
      device_released = true;
    });
  };
  RunResult result;
  std::thread execution([&] {
    result = registry.ExecuteWithHooks("test.primitive_lifecycle", "1.0.0",
                                       nlohmann::json::object(), "cancel-drain",
                                       std::move(hooks));
  });
  {
    std::unique_lock lock(mutex);
    changed.wait(lock, [&] { return primitive_started; });
  }
  manager_cancelled = true;
  execution.join();

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(primitive_cancelled);
  EXPECT_TRUE(primitive_terminal);
  EXPECT_TRUE(device_released);
}

TEST_F(PrimitiveLifecycleTest, DeviceLeaseCoversPrimitiveThroughTerminalReturn) {
  bool acquired = false;
  bool released = false;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json, const std::string&, Cancelled) {
    EXPECT_TRUE(acquired);
    EXPECT_FALSE(released);
    return nlohmann::json{{"accepted", true},
                          {"result", {{"status", "outcome_unknown"}}}};
  };
  Registry registry(timer_, std::move(capabilities));
  Definition definition;
  definition.metadata.name = "test.device_lease";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Exercises per-primitive device leases";
  definition.metadata.parameters_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.returns_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.primitives.push_back({"test.operation", "1.0.0"});
  definition.metadata.reentrant = true;
  definition.handler = [](Execution& execution) -> Task<Outcome> {
    auto operation = execution.StartPrimitive(
        {"test.operation", "1.0.0"}, "device-0", nlohmann::json::object());
    if (!operation) co_return Outcome::Failed("start.failed", operation.error().message);
    auto terminal = execution.WaitPrimitive(operation.value(), 1s);
    if (!terminal || !terminal.value())
      co_return Outcome::Failed("wait.failed", "primitive did not reach terminal state");
    co_return Outcome::Failed("expected.outcome_unknown", "primitive outcome is unknown");
  };
  registry.Register(std::move(definition));

  ExecutionHooks hooks;
  hooks.acquire_device = [&](const std::string& device_id) {
    EXPECT_EQ(device_id, "device-0");
    acquired = true;
    return std::function<void()>([&] { released = true; });
  };
  const RunResult result = registry.ExecuteWithHooks(
      "test.device_lease", "1.0.0", nlohmann::json::object(), "device-lease",
      std::move(hooks));

  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_TRUE(acquired);
  EXPECT_TRUE(released);
}

TEST_F(PrimitiveLifecycleTest, CancelWhileWaitingForDeviceNeverReleasesPrimitive) {
  std::mutex mutex;
  std::condition_variable changed;
  bool acquiring = false;
  bool allow_acquire = false;
  std::atomic<int> backend_calls{0};
  std::atomic<int> cancel_calls{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json, const std::string&, Cancelled) {
    ++backend_calls;
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [&](const std::string&) { ++cancel_calls; };
  Registry registry(timer_, std::move(capabilities));
  Definition definition;
  definition.metadata.name = "test.cancel_before_release";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Cancellation while device acquisition is pending";
  definition.metadata.parameters_schema = {{"type", "object"}, {"additionalProperties", false}};
  definition.metadata.returns_schema = {{"type", "object"}};
  definition.metadata.primitives.push_back({"base.navigate", "1.0.0"});
  definition.metadata.reentrant = true;
  definition.handler = [&](Execution& execution) -> Task<Outcome> {
    auto operation = execution.StartPrimitive(
        {"base.navigate", "1.0.0"}, "base-0", nlohmann::json::object());
    if (!operation) co_return Outcome::Failed("start.failed", operation.error().message);
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, 2s, [&] { return acquiring; })) {
        allow_acquire = true;
        lock.unlock();
        changed.notify_all();
        co_return Outcome::Failed("acquire.timeout", "device acquisition did not start");
      }
    }
    auto cancelled = execution.CancelPrimitive(operation.value());
    if (!cancelled) co_return Outcome::Failed("cancel.failed", cancelled.error().message);
    {
      std::lock_guard lock(mutex);
      allow_acquire = true;
    }
    changed.notify_all();
    auto terminal = execution.WaitPrimitive(operation.value(), 2s);
    if (!terminal || !terminal.value())
      co_return Outcome::Failed("wait.failed", "cancelled primitive did not settle");
    if (terminal.value()->at("result").at("status") != "cancelled")
      co_return Outcome::Failed("unexpected.status", terminal.value()->dump());
    co_return Outcome::Completed();
  };
  registry.Register(std::move(definition));
  ExecutionHooks hooks;
  hooks.acquire_device = [&](const std::string& device) {
    EXPECT_EQ(device, "base-0");
    std::unique_lock lock(mutex);
    acquiring = true;
    changed.notify_all();
    changed.wait(lock, [&] { return allow_acquire; });
    return std::function<void()>([] {});
  };
  const auto result = registry.ExecuteWithHooks(
      "test.cancel_before_release", "1.0.0", nlohmann::json::object(),
      "cancel-before-release", std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(cancel_calls, 1);
  EXPECT_EQ(backend_calls, 0);
}

TEST_F(PrimitiveLifecycleTest, NavigateWaypointDistinguishesTerminalPrimitiveStatuses) {
  struct Case {
    bool accepted;
    const char* status;
    const char* reason_code;
    RunResult::Status run_status;
  };
  constexpr Case cases[] = {{true, "failed", "navigation.failed", RunResult::Status::kFailed},
                            {true, "cancelled", "cancelled", RunResult::Status::kCancelled},
                            {false, "cancelled", "cancelled", RunResult::Status::kCancelled},
                            {true, "timed_out", "navigation.timed_out", RunResult::Status::kFailed},
                            {true, "outcome_unknown", "navigation.outcome_unknown",
                              RunResult::Status::kFailed}};
  for (const auto& test : cases) {
    RuntimeCapabilities capabilities;
    capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                         nlohmann::json, const std::string&, Cancelled) {
      return nlohmann::json{{"accepted", test.accepted},
                            {"result", {{"status", test.status}, {"reason", "detail"}}}};
    };
    capabilities.cancel_primitive = [](const std::string&) {};
    Registry registry(timer_, std::move(capabilities));
    island::skills::native::RegisterSkills(registry);
    const RunResult result = registry.Execute(
        "navigate_waypoint", "1.0.0",
        {{"robot_name", "robot-0"},
         {"base_device_id", "base-0"},
         {"waypoint_id", "target"},
         {"target", {{"reference", "relative"}, {"x_m", 1.0}, {"y_m", 0.0},
                     {"yaw_rad", 0.0}}}},
        std::string("terminal-") + test.status);
    EXPECT_EQ(result.status, test.run_status) << test.status;
    EXPECT_EQ(result.reason_code, test.reason_code) << test.status;
    EXPECT_EQ(result.message, "detail") << test.status;
  }
}

TEST_F(PrimitiveLifecycleTest, RegisterHostIgnoresSpeakerTranscriptAndRegistersSpokenNames) {
  std::vector<std::string> spoken;
  std::vector<std::string> registered;
  std::uint64_t publication_sequence = 0;
  std::size_t host = 0;
  bool emit_prompt_transcript = true;
  bool third_host_answered_during_prompt = false;
  constexpr const char* kNames[] = {"张三", "李四", "Alice"};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       nlohmann::json arguments, const std::string&, Cancelled) {
    if (primitive.name == "speaker.speak") {
      spoken.push_back(arguments.value("text", ""));
      if (arguments.value("text", "") == "请自我介绍" && host == 2) {
        ++publication_sequence;
        third_host_answered_during_prompt = true;
      }
    } else if (primitive.name == "face.register") {
      registered.push_back(arguments.value("name", ""));
    }
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.open_resource_subscription = [&](const std::string& uri) {
    EXPECT_EQ(uri, "robots/demo-0/world/asr/state");
    RuntimeCapabilities::ResourceAccess access;
    access.lifetime = std::make_shared<int>(1);
    access.read_history = [&](const ResourceSelector& selector) {
      EXPECT_EQ(selector.kind, ResourceSelectorKind::kLatest);
      return ResourceHistory{{}, {0, publication_sequence, 0, false}, false, false};
    };
    access.wait_publication = [&](std::uint64_t after, std::chrono::milliseconds timeout,
                                  Cancelled) {
      EXPECT_EQ(timeout, 50ms);
      if (third_host_answered_during_prompt) {
        EXPECT_EQ(after + 1, publication_sequence);
        third_host_answered_during_prompt = false;
        ++host;
        return ResourceWait{ResourceWaitStatus::kPublication,
                            ResourcePublication{publication_sequence, {}, "boot", "epoch",
                                                {{"text", "My name is Alice"}}},
                            {1, publication_sequence, 0, false}, false};
      }
      EXPECT_EQ(after, publication_sequence);
      ++publication_sequence;
      std::string text;
      if (emit_prompt_transcript) {
        text = "请自我介绍";
        emit_prompt_transcript = false;
      } else {
        text = host == 2 ? "My name is Alice" : std::string("我是") + kNames[host];
        ++host;
        emit_prompt_transcript = true;
      }
      return ResourceWait{ResourceWaitStatus::kPublication,
                          ResourcePublication{publication_sequence, {}, "boot", "epoch",
                                              {{"text", text}}},
                          {1, publication_sequence, 0, false}, false};
    };
    return access;
  };
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);

  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  const RunResult result = registry.ExecuteWithHooks(
      "register_host", "1.0.0",
      {{"robot_name", "demo-0"},
       {"camera_device_id", "cam-0"},
       {"speaker_device_id", "speaker-0"}},
      "register-host-asr-boundary", std::move(hooks));

  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code << ": "
                                                         << result.message;
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(registered, (std::vector<std::string>{"张三", "李四", "Alice"}));
  EXPECT_EQ(spoken, (std::vector<std::string>{"请主人1就位", "请自我介绍", "开始注册",
                                               "注册完成", "请主人2就位", "请自我介绍",
                                               "开始注册", "注册完成", "请主人3就位",
                                               "请自我介绍", "开始注册", "注册完成",
                                               "三个主人全部注册完成"}));
}

TEST_F(PrimitiveLifecycleTest, RegisterHostContinuesAfterHostFailures) {
  std::vector<std::string> spoken;
  std::vector<std::string> attempted_registrations;
  std::uint64_t publication_sequence = 0;
  std::size_t stage = 0;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string&, nlohmann::json arguments,
          const std::string&, Cancelled) {
        if (primitive.name == "speaker.speak") {
          spoken.push_back(arguments.value("text", ""));
        } else if (primitive.name == "face.register") {
          const std::string name = arguments.value("name", "");
          attempted_registrations.push_back(name);
          if (name == "李四")
            return nlohmann::json{{"accepted", true},
                                  {"result", {{"status", "failed"}, {"reason", "no face"}}}};
        }
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.open_resource_subscription = [&](const std::string&) {
    RuntimeCapabilities::ResourceAccess access;
    access.lifetime = std::make_shared<int>(1);
    access.read_history = [&](const ResourceSelector&) {
      ++stage;
      return ResourceHistory{{}, {0, publication_sequence, 0, false}, false, false};
    };
    access.wait_publication = [&](std::uint64_t after, std::chrono::milliseconds timeout,
                                  Cancelled) {
      if (stage == 1) {
        std::this_thread::sleep_for(timeout);
        return ResourceWait{ResourceWaitStatus::kTimeout, std::nullopt, {}, false};
      }
      EXPECT_EQ(after, publication_sequence);
      ++publication_sequence;
      const std::string text = stage == 2 ? "我是李四" : "My name is Alice";
      return ResourceWait{ResourceWaitStatus::kPublication,
                          ResourcePublication{publication_sequence, {}, "boot", "epoch",
                                              {{"text", text}}},
                          {1, publication_sequence, 0, false}, false};
    };
    return access;
  };
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);

  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  const RunResult result = registry.ExecuteWithHooks(
      "register_host", "1.0.0",
      {{"robot_name", "demo-0"}, {"camera_device_id", "cam-0"},
       {"speaker_device_id", "speaker-0"}, {"introduction_timeout_ms", 1000}},
      "register-host-partial", std::move(hooks));

  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(attempted_registrations, (std::vector<std::string>{"李四", "Alice"}));
  EXPECT_EQ(result.result.at("registered_hosts"), nlohmann::json::array({"Alice"}));
  EXPECT_EQ(spoken.back(), "主人注册流程已完成");
}

TEST_F(PrimitiveLifecycleTest, InteractFallenOwnerExecutesAssistanceArmMoveJ) {
  std::uint64_t sequence = 0;
  std::vector<std::string> spoken;
  std::string movej_device;
  std::vector<nlohmann::json> movej_arguments;
  std::vector<std::string> arm_primitives;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/arm/state")
      return nlohmann::json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                            {"joint_valid", {true, true, true, true}}};
    if (uri == "robots/demo-0/base/status")
      return nlohmann::json{{"telemetry_valid", true}, {"fresh", true},
                            {"pose_valid", true}, {"localization_valid", true},
                            {"frame_id", "map"},
                            {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    if (uri == "robots/demo-0/detection/head/state")
      return nlohmann::json{{"sequence", ++sequence}, {"detections", nlohmann::json::array()}};
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    return WavingOwner(++sequence, 0.8, "Alice", "human:1", "fallen", "lying_on_floor", "lie");
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string& device_id,
          nlohmann::json arguments, const std::string&, Cancelled) {
        if (primitive.name == "arm.movej") {
          movej_device = device_id;
          movej_arguments.push_back(std::move(arguments));
          arm_primitives.push_back(primitive.name);
        } else if (primitive.name == "arm.init") {
          EXPECT_EQ(device_id, "arm-0");
          EXPECT_TRUE(arguments.empty());
          arm_primitives.push_back(primitive.name);
        } else if (primitive.name == "speaker.speak") {
          spoken.push_back(arguments.value("text", ""));
        } else {
          EXPECT_EQ(primitive.name, "base.navigate");
        }
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  nlohmann::json arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0", std::move(arguments), "fallen-owner",
      std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.result.at("name"), "Alice");
  EXPECT_EQ(result.result.at("action"), "fallen");
  EXPECT_TRUE(result.result.at("actuation_performed").get<bool>());
  EXPECT_EQ(spoken, (std::vector<std::string>{"识别到主人Alice",
                                               "当前姿态为躺卧，状态为倒地",
                                               "请不要担心，我来帮助您"}));
  EXPECT_EQ(movej_device, "arm-0");
  EXPECT_EQ(arm_primitives, (std::vector<std::string>{"arm.movej", "arm.movej", "arm.init"}));
  ASSERT_EQ(movej_arguments.size(), 2u);
  EXPECT_EQ(movej_arguments.at(0).at("reference"), "absolute");
  EXPECT_EQ(movej_arguments.at(0).at("joint_positions_rad"),
            (nlohmann::json{0.1, 0.2, 0.3, 0.4, 0.5, 0.6}));
  EXPECT_EQ(movej_arguments.at(1).at("reference"), "absolute");
  EXPECT_EQ(movej_arguments.at(1).at("joint_positions_rad"),
            (nlohmann::json{0.0, 0.1, 0.2, 0.3, 0.4, 0.5}));
}

TEST_F(PrimitiveLifecycleTest, InteractFallenOwnerRecoversFromTransientInvalidFrame) {
  std::uint64_t sequence = 0;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/arm/state")
      return nlohmann::json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                            {"joint_valid", {true, true, true, true}}};
    if (uri == "robots/demo-0/base/status")
      return nlohmann::json{{"telemetry_valid", true}, {"fresh", true},
                            {"pose_valid", true}, {"localization_valid", true},
                            {"frame_id", "map"},
                            {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    if (uri == "robots/demo-0/detection/head/state")
      return nlohmann::json{{"sequence", ++sequence}, {"detections", nlohmann::json::array()}};
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    auto snapshot = WavingOwner(++sequence, 0.8, "Alice", "human:1", "fallen",
                                "lying_on_floor", "lie");
    if (sequence <= 2) snapshot["detections"][0]["bbox"] = {100, 100, 200, 700};
    if (sequence == 3) snapshot["detections"][0]["confidence"] = 0.5;
    return snapshot;
  };
  capabilities.execute_primitive =
      [](const PrimitiveReference& primitive, const std::string&, nlohmann::json,
         const std::string&, Cancelled) {
        EXPECT_TRUE(primitive.name == "speaker.speak" || primitive.name == "base.navigate" ||
                    primitive.name == "arm.movej" || primitive.name == "arm.init");
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  nlohmann::json arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0", std::move(arguments),
      "fallen-owner-transient-frame", std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_GE(sequence, 4u);
  EXPECT_EQ(result.result.at("name"), "Alice");
  EXPECT_EQ(result.result.at("action"), "fallen");
}

TEST_F(PrimitiveLifecycleTest, InteractFallenOwnerLatchesScenarioDuringAlignment) {
  std::uint64_t sequence = 0;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/arm/state")
      return nlohmann::json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                            {"joint_valid", {true, true, true, true}}};
    if (uri == "robots/demo-0/base/status")
      return nlohmann::json{{"telemetry_valid", true}, {"fresh", true},
                            {"pose_valid", true}, {"localization_valid", true},
                            {"frame_id", "map"},
                            {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    auto snapshot = WavingOwner(++sequence, 0.8, "Alice", "human:1",
                                sequence <= 2 ? "fallen" : "none",
                                sequence <= 2 ? "lying_on_floor" : "standing",
                                sequence <= 2 ? "lie" : "stand");
    if (sequence <= 2) snapshot["detections"][0]["bbox"] = {100, 100, 200, 700};
    return snapshot;
  };
  capabilities.execute_primitive =
      [](const PrimitiveReference& primitive, const std::string&, nlohmann::json,
         const std::string&, Cancelled) {
        EXPECT_TRUE(primitive.name == "base.navigate" || primitive.name == "arm.movej" ||
                    primitive.name == "arm.init");
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  auto arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  arguments["speech_enabled"] = false;
  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0", std::move(arguments),
      "fallen-owner-latched-scenario", std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_GE(sequence, 3u);
  EXPECT_EQ(result.result.at("action"), "fallen");
  EXPECT_EQ(result.result.at("posture_state"), "lying_on_floor");
}

TEST_F(PrimitiveLifecycleTest, InteractFallenOwnerFailsWhenAssistanceArmRejected) {
  std::uint64_t sequence = 0;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/arm/state")
      return nlohmann::json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                            {"joint_valid", {true, true, true, true}}};
    if (uri == "robots/demo-0/base/status")
      return nlohmann::json{{"telemetry_valid", true}, {"fresh", true},
                            {"pose_valid", true}, {"localization_valid", true},
                            {"frame_id", "map"},
                            {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    if (uri == "robots/demo-0/detection/head/state")
      return nlohmann::json{{"sequence", ++sequence}, {"detections", nlohmann::json::array()}};
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    return WavingOwner(++sequence, 0.8, "Alice", "human:1", "fallen", "lying_on_floor", "lie");
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string&, nlohmann::json,
         const std::string&, Cancelled) {
        if (primitive.name == "arm.movej")
          return nlohmann::json{{"accepted", false}, {"result", {{"status", "rejected"}}}};
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  nlohmann::json arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0", std::move(arguments), "fallen-owner-movej-rejected",
      std::move(hooks));

  EXPECT_EQ(result.status, RunResult::Status::kFailed);
  EXPECT_EQ(result.reason_code, "assistance_arm.failed");
}

TEST_F(PrimitiveLifecycleTest, InteractFallenOwnerRunsAssistanceArmWhenSpeechDisabled) {
  std::uint64_t sequence = 0;
  bool movej_called = false;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/detection/head/state")
      return nlohmann::json{{"source_sequence", sequence + 1},
                            {"detections", nlohmann::json::array()}};
    if (uri == "robots/demo-0/arm/state")
      return nlohmann::json{{"joint_positions_rad", {0.0, 0.0, 0.0, 0.0}},
                            {"joint_valid", {true, true, true, true}}};
    if (uri == "robots/demo-0/base/status")
      return nlohmann::json{{"telemetry_valid", true}, {"fresh", true},
                            {"pose_valid", true}, {"localization_valid", true},
                            {"frame_id", "map"},
                            {"pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}}};
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    return WavingOwner(++sequence, 0.8, "Alice", "human:1", "fallen", "lying_on_floor", "lie");
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string&, nlohmann::json,
         const std::string&, Cancelled) {
        EXPECT_TRUE(primitive.name == "base.navigate" || primitive.name == "arm.movej" ||
                    primitive.name == "arm.init");
        if (primitive.name == "arm.movej") movej_called = true;
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  nlohmann::json arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  arguments["speech_enabled"] = false;
  const RunResult result = registry.ExecuteWithHooks(
      "interact_fallen_owner", "1.0.0", std::move(arguments), "fallen-owner-speech-disabled",
      std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(movej_called);
  EXPECT_TRUE(result.result.at("actuation_performed").get<bool>());
}

TEST_F(PrimitiveLifecycleTest, InteractFurnitureOwnerAcceptsCanonicalSittingPose) {
  std::uint64_t sequence = 0;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    return WavingOwner(++sequence, 0.8, "Alice", "human:1", "rest", "sitting", "sit");
  };
  capabilities.execute_primitive =
      [](const PrimitiveReference&, const std::string&, nlohmann::json, const std::string&,
         Cancelled) {
        ADD_FAILURE() << "no primitive should run when speech is disabled and the owner is near";
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  auto arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  arguments["speech_enabled"] = false;
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0", std::move(arguments),
      "furniture-owner-canonical-pose", std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.result.at("pose"), "sit");
  EXPECT_EQ(result.result.at("posture_state"), "sitting");
}

TEST_F(PrimitiveLifecycleTest, InteractFurnitureOwnerCompletesAfterPromptWithoutWaitingForASR) {
  std::uint64_t detection_sequence = 0;
  std::uint64_t asr_reads = 0;
  std::vector<std::string> spoken;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri == "robots/demo-0/world/asr/state") {
      ++asr_reads;
      return nlohmann::json{{"sequence", asr_reads == 1 ? 7 : 8},
                            {"text", asr_reads == 1 ? "旧指令" : "请帮我打开开关"}};
    }
    EXPECT_EQ(uri, "robots/demo-0/detection/state");
    return WavingOwner(++detection_sequence, 0.8, "Alice", "human:1", "rest",
                       "lying_on_furniture", "lie");
  };
  capabilities.execute_primitive =
      [&](const PrimitiveReference& primitive, const std::string&, nlohmann::json arguments,
          const std::string&, Cancelled) {
        EXPECT_EQ(primitive.name, "speaker.speak");
        spoken.push_back(arguments.value("text", ""));
        return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
      };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  island::skills::native::RegisterSkills(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  auto arguments = ApproachOwnerArguments();
  arguments["arm_device_id"] = "arm-0";
  arguments["assistance_joint_positions_rad"] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  arguments["reset_joint_positions_rad"] = {0.0, 0.1, 0.2, 0.3, 0.4, 0.5};
  arguments["switch_waypoint_id"] = "switch";
  arguments["switch_joint_positions_rad"] = {0.2, 0.3, 0.4, 0.5, 0.6, 0.7};
  const RunResult result = registry.ExecuteWithHooks(
      "interact_furniture_owner", "1.0.0", std::move(arguments), "furniture-owner",
      std::move(hooks));

  ASSERT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_TRUE(result.accepted);
  EXPECT_EQ(result.result.at("posture_state"), "lying_on_furniture");
  EXPECT_TRUE(result.result.at("recognized_text").is_null());
  EXPECT_TRUE(result.result.at("requested_state").is_null());
  EXPECT_FALSE(result.result.at("actuation_performed").get<bool>());
  EXPECT_EQ(asr_reads, 0u);
  EXPECT_EQ(spoken, (std::vector<std::string>{"识别到主人Alice",
                                               "当前姿态为躺卧，状态为休息",
                                               "请问你是要打开还是要关闭开关"}));
}

}  // namespace
}  // namespace eos::runtime::skills::builtin

#include "share/skills/native/head_find_trash_navigation_resume/head_find_trash_navigation_resume.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::head_find_trash_navigation_resume {
namespace {
using namespace std::chrono_literals;
using namespace eos::runtime::skills;
using namespace eos::runtime::skills::builtin;
using Json = nlohmann::json;

Json Target(double x) {
  return {
      {"reference", "absolute"}, {"frame_id", "map"}, {"x_m", x}, {"y_m", 2.0}, {"yaw_rad", 0.5}};
}

Json Arguments(bool with_navigation = true) {
  Json arguments{
      {"robot_name", "demo-0"}, {"speaker_device_id", "speaker-0"}, {"base_device_id", "base-0"}};
  if (with_navigation) {
    arguments["navigation_command_id"] = "external-navigation";
    arguments["resume_target"] = Target(1.0);
  }
  return arguments;
}

Json Snapshot(std::uint64_t sequence, bool present = true,
              const std::string& label = "bottle") {
  Json detections = Json::array();
  if (present) {
    detections.push_back({{"kind", "object"}, {"label", label}, {"confidence", 0.9}});
  }
  return {{"source_sequence", sequence}, {"detections", std::move(detections)}};
}

Json Ack(const char* status = "succeeded") {
  return {{"accepted", true}, {"result", {{"status", status}}}};
}

class NavigationResumeTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

TEST_F(NavigationResumeTest, ValidatesInitialBindingAndDeclaresUpdateSignal) {
  Registry registry(timer_);
  RegisterSkill(registry);
  const auto* metadata = registry.Find("head_find_trash_navigation_resume", "1.0.0");
  ASSERT_NE(metadata, nullptr);
  ASSERT_EQ(metadata->accepted_signals.size(), 1u);
  EXPECT_EQ(metadata->accepted_signals.front().name, "navigation.update");
  EXPECT_NO_THROW(registry.ValidateArguments(metadata->name, metadata->version, Arguments()));
  EXPECT_NO_THROW(registry.ValidateArguments(metadata->name, metadata->version, Arguments(false)));
  Json missing_target = Arguments();
  missing_target.erase("resume_target");
  EXPECT_THROW(registry.ValidateArguments(metadata->name, metadata->version, missing_target),
               std::exception);
  Json relative = Arguments();
  relative["resume_target"]["reference"] = "relative";
  EXPECT_THROW(registry.ValidateArguments(metadata->name, metadata->version, relative),
               std::exception);
  Json extra = Arguments();
  extra["resume_target"]["extra"] = true;
  EXPECT_THROW(registry.ValidateArguments(metadata->name, metadata->version, extra),
               std::exception);
}

TEST_F(NavigationResumeTest, UpdatesTargetAndStartsPlaybackBeforeCancellationCompletes) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<bool> cancel_called{false};
  std::atomic<bool> playback_started{false};
  std::atomic<int> resumed{0};
  std::atomic<int> owned_cancelled{0};
  std::mutex mutex;
  Json resumed_target;
  std::vector<std::string> checkpoints;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    EXPECT_EQ(uri, "robots/demo-0/detection/head/state");
    return Snapshot(++reads, true, "empty_box");
  };
  capabilities.cancel_navigation_command = [&](const std::string& command,
                                               const std::string& device) {
    EXPECT_EQ(command, "replacement-navigation");
    EXPECT_EQ(device, "base-0");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!playback_started && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(1ms);
    EXPECT_TRUE(playback_started.load());
    cancel_called = true;
    return NavigationCancelStatus::kCancelled;
  };
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive,
                                       const std::string& device, Json arguments,
                                       const std::string&, Cancelled) {
    if (primitive.name == "speaker.play") {
      EXPECT_EQ(device, "speaker-0");
      EXPECT_EQ(arguments.at("uri"), "asset://head_find_trash/found_empty_box.wav");
      playback_started = true;
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!cancel_called && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      return Ack();
    }
    EXPECT_EQ(primitive.name, "base.navigate");
    EXPECT_EQ(device, "base-0");
    {
      std::lock_guard lock(mutex);
      resumed_target = std::move(arguments);
    }
    ++resumed;
    return Ack("cancelled");
  };
  capabilities.cancel_primitive = [&](const std::string&) { ++owned_cancelled; };
  Registry registry(timer_, std::move(capabilities));
  RegisterSkill(registry);
  std::atomic<int> signal_index{0};
  ExecutionHooks hooks;
  hooks.receive_signal = [&](std::chrono::milliseconds) -> std::optional<Signal> {
    const int index = signal_index.fetch_add(1);
    if (index == 0) return Signal{"navigation.update", {{"navigation_command_id", "bad"}}};
    if (index == 1)
      return Signal{
          "navigation.update",
          {{"navigation_command_id", "replacement-navigation"}, {"resume_target", Target(7.0)}}};
    return std::nullopt;
  };
  hooks.emit_checkpoint = [&](const Checkpoint& checkpoint) {
    std::lock_guard lock(mutex);
    checkpoints.push_back(checkpoint.name + ":" + checkpoint.message);
    return true;
  };
  hooks.cancelled = [&] { return resumed.load() > 0; };
  const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                Arguments(), "navigation-resume-update", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled) << result.reason_code;
  EXPECT_TRUE(cancel_called);
  EXPECT_TRUE(playback_started);
  EXPECT_EQ(resumed, 1);
  EXPECT_GE(owned_cancelled, 1);
  {
    std::lock_guard lock(mutex);
    EXPECT_EQ(resumed_target, Target(7.0));
    EXPECT_NE(std::find(checkpoints.begin(), checkpoints.end(),
                        "navigation.warning:navigation.binding_invalid"),
              checkpoints.end());
    EXPECT_NE(std::find(checkpoints.begin(), checkpoints.end(), "navigation.updated:"),
              checkpoints.end());
  }
}

TEST_F(NavigationResumeTest, MissingAndMismatchedCommandsNeverResume) {
  for (const auto status : {NavigationCancelStatus::kNotFound, NavigationCancelStatus::kMismatch,
                            NavigationCancelStatus::kUncertain}) {
    std::atomic<std::uint64_t> reads{0};
    std::atomic<int> speeches{0};
    std::atomic<int> navigation_calls{0};
    std::mutex mutex;
    std::vector<std::string> warnings;
    RuntimeCapabilities capabilities;
    capabilities.read_resource = [&](const std::string&) { return Snapshot(++reads); };
    capabilities.cancel_navigation_command = [&](const std::string& command,
                                                 const std::string& device) {
      EXPECT_EQ(command, "external-navigation");
      EXPECT_EQ(device, "base-0");
      return status;
    };
    capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                         Json, const std::string&, Cancelled) {
      if (primitive.name == "speaker.play") ++speeches;
      if (primitive.name == "base.navigate") ++navigation_calls;
      return Ack();
    };
    Registry registry(timer_, std::move(capabilities));
    RegisterSkill(registry);
    ExecutionHooks hooks;
    hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
      return std::nullopt;
    };
    hooks.emit_checkpoint = [&](const Checkpoint& checkpoint) {
      if (checkpoint.name == "navigation.warning") {
        std::lock_guard lock(mutex);
        warnings.push_back(checkpoint.message);
      }
      return true;
    };
    hooks.cancelled = [&] { return reads.load() >= 7; };
    const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                  Arguments(), "navigation-resume-missing", hooks);
    EXPECT_EQ(result.status, RunResult::Status::kCancelled);
    EXPECT_EQ(speeches, 1);
    EXPECT_EQ(navigation_calls, 0);
    std::lock_guard lock(mutex);
    EXPECT_NE(
        std::find(warnings.begin(), warnings.end(),
                  status == NavigationCancelStatus::kNotFound   ? "navigation.command_not_found"
                  : status == NavigationCancelStatus::kMismatch ? "navigation.command_mismatch"
                                                                : "navigation.cancel_uncertain"),
        warnings.end());
  }
}

TEST_F(NavigationResumeTest, PlaybackFailureStillResumesAndLaterSightingCancelsOwnedNavigation) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<int> announcements{0};
  std::atomic<int> resumes{0};
  std::atomic<int> owned_cancelled{0};
  std::mutex mutex;
  std::condition_variable changed;
  bool release_owned = false;
  std::vector<std::string> warnings;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string&) {
    const auto sequence = ++reads;
    return Snapshot(sequence, sequence <= 3 || sequence >= 14);
  };
  capabilities.cancel_navigation_command = [&](const std::string& command, const std::string&) {
    if (command != "external-navigation") {
      {
        std::lock_guard lock(mutex);
        release_owned = true;
      }
      changed.notify_all();
    }
    return NavigationCancelStatus::kCancelled;
  };
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json, const std::string&, Cancelled) {
    if (primitive.name == "speaker.play") {
      return ++announcements == 1 ? Ack("failed") : Ack();
    }
    ++resumes;
    std::unique_lock lock(mutex);
    changed.wait_for(lock, 3s, [&] { return release_owned; });
    release_owned = false;
    return Ack("cancelled");
  };
  capabilities.cancel_primitive = [&](const std::string&) {
    ++owned_cancelled;
    {
      std::lock_guard lock(mutex);
      release_owned = true;
    }
    changed.notify_all();
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterSkill(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.emit_checkpoint = [&](const Checkpoint& checkpoint) {
    if (checkpoint.name == "navigation.warning") {
      std::lock_guard lock(mutex);
      warnings.push_back(checkpoint.message);
    }
    return true;
  };
  hooks.cancelled = [&] { return resumes.load() >= 2; };
  const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                Arguments(), "navigation-resume-repeat", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled) << result.reason_code;
  EXPECT_EQ(announcements, 2);
  EXPECT_EQ(resumes, 2);
  EXPECT_GE(owned_cancelled, 1);
  std::lock_guard lock(mutex);
  EXPECT_NE(std::find(warnings.begin(), warnings.end(), "announcement.failed"), warnings.end());
}

TEST_F(NavigationResumeTest, OwnsForwardNavigationAndCompletesAtMapDestination) {
  RuntimeCapabilities capabilities;
  Json navigation_target;
  std::atomic<int> navigation_calls{0};
  capabilities.read_resource = [](const std::string& uri) -> Json {
    if (uri == "robots/demo-0/base/status")
      return Json{
          {"telemetry_valid", true}, {"fresh", true},
          {"pose_valid", true},      {"localization_valid", true},
          {"frame_id", "map"},       {"pose", {{"x_m", 1.0}, {"y_m", 2.0}, {"yaw_rad", 0.0}}}};
    return Snapshot(1, false);
  };
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json arguments, const std::string&, Cancelled) {
    EXPECT_EQ(primitive.name, "base.navigate");
    navigation_target = std::move(arguments);
    ++navigation_calls;
    return Ack();
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterSkill(registry);
  Json arguments = Arguments(false);
  arguments["start_relative_target"] = {{"x_m", 3.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}};
  EXPECT_NO_THROW(
      registry.ValidateArguments("head_find_trash_navigation_resume", "1.0.0", arguments));
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                arguments, "owned-forward-navigation", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(navigation_calls, 1);
  EXPECT_EQ(navigation_target, (Json{{"reference", "absolute"},
                                     {"frame_id", "map"},
                                     {"x_m", 4.0},
                                     {"y_m", 2.0},
                                     {"yaw_rad", 0.0}}));
  EXPECT_EQ(result.result.value("destination_reached", false), true);
}

TEST_F(NavigationResumeTest, OwnsWaypointNavigationAndCompletesAtMapDestination) {
  const Json target = Target(6.0);
  Json navigation_target;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) -> Json {
    if (uri == "robots/demo-0/navigation/waypoints/demo_exit") return target;
    EXPECT_EQ(uri, "robots/demo-0/detection/head/state");
    return Snapshot(1, false);
  };
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json arguments, const std::string&, Cancelled) {
    EXPECT_EQ(primitive.name, "base.navigate");
    navigation_target = std::move(arguments);
    return Ack();
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterSkill(registry);
  Json arguments = Arguments(false);
  arguments["start_waypoint_id"] = "demo_exit";
  EXPECT_NO_THROW(
      registry.ValidateArguments("head_find_trash_navigation_resume", "1.0.0", arguments));
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                arguments, "owned-waypoint-navigation", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(navigation_target, target);
  EXPECT_EQ(result.result.value("destination_reached", false), true);
}

TEST_F(NavigationResumeTest, OwnedNavigationResumesAfterConfirmedStopWithFailedVendorTerminal) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<int> navigation_calls{0};
  std::atomic<int> announcements{0};
  std::mutex mutex;
  std::condition_variable changed;
  bool release_initial = false;
  std::vector<Json> targets;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) -> Json {
    if (uri == "robots/demo-0/base/status")
      return {{"telemetry_valid", true}, {"fresh", true},
              {"pose_valid", true},      {"localization_valid", true},
              {"frame_id", "map"},       {"pose", {{"x_m", 1.0}, {"y_m", 2.0}, {"yaw_rad", 0.0}}}};
    return Snapshot(++reads);
  };
  capabilities.execute_primitive = [&](const PrimitiveReference& primitive, const std::string&,
                                       Json arguments, const std::string&, Cancelled) {
    if (primitive.name == "speaker.play") {
      ++announcements;
      return Ack();
    }
    {
      std::lock_guard lock(mutex);
      targets.push_back(std::move(arguments));
    }
    if (++navigation_calls == 1) {
      std::unique_lock lock(mutex);
      changed.wait_for(lock, 3s, [&] { return release_initial; });
      return Ack("failed");
    }
    return Ack();
  };
  capabilities.cancel_navigation_command = [&](const std::string& command,
                                               const std::string& device) {
    EXPECT_FALSE(command.empty());
    EXPECT_EQ(device, "base-0");
    {
      std::lock_guard lock(mutex);
      release_initial = true;
    }
    changed.notify_all();
    return NavigationCancelStatus::kCancelled;
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterSkill(registry);
  Json arguments = Arguments(false);
  arguments["start_relative_target"] = {{"x_m", 3.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}};
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  const auto result = registry.ExecuteWithHooks("head_find_trash_navigation_resume", "1.0.0",
                                                arguments, "owned-trash-resume", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(announcements, 1);
  EXPECT_EQ(navigation_calls, 2);
  ASSERT_EQ(targets.size(), 2u);
  EXPECT_EQ(targets[0], targets[1]);
  EXPECT_EQ(targets[0], (Json{{"reference", "absolute"},
                              {"frame_id", "map"},
                              {"x_m", 4.0},
                              {"y_m", 2.0},
                              {"yaw_rad", 0.0}}));
}

}  // namespace
}  // namespace island::skills::native::head_find_trash_navigation_resume

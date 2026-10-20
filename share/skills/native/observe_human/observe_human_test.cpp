#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <iterator>
#include <mutex>
#include <numeric>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include "src/eos/lib/timer.h"
#include "src/eos/runtime/skills/builtin/registry.h"
#include "share/skills/native/observe_human/geometry.h"

namespace island::skills::native {
void RegisterObserveHuman(::eos::runtime::skills::builtin::Registry& registry);
namespace {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;

class ObserveHumanTest : public testing::Test {
 protected:
  eos::lib::ThreadTimerService timer_;
};

TEST(ObserveHumanGeometryTest, ConvertsNormalizedImageErrorToCameraRayAngle) {
  const double half_horizontal_fov = detail::kHorizontalFovRad / 2.0;
  EXPECT_DOUBLE_EQ(detail::NormalizedImageErrorToAngle(0.0, detail::kHorizontalFovRad), 0.0);
  EXPECT_NEAR(detail::NormalizedImageErrorToAngle(1.0, detail::kHorizontalFovRad),
              half_horizontal_fov, 1e-12);
  EXPECT_NEAR(detail::NormalizedImageErrorToAngle(-1.0, detail::kHorizontalFovRad),
              -half_horizontal_fov, 1e-12);
  EXPECT_GT(detail::NormalizedImageErrorToAngle(0.25, detail::kHorizontalFovRad), 0.0);
  EXPECT_LT(detail::NormalizedImageErrorToAngle(-0.25, detail::kHorizontalFovRad), 0.0);
}

TEST(ObserveHumanGeometryTest, DerivesVerticalFovFromFrameAspectRatio) {
  const double vertical_fov =
      detail::VerticalFovRad(detail::kHorizontalFovRad, 640.0, 480.0);
  const double expected =
      2.0 * std::atan(std::tan(detail::kHorizontalFovRad / 2.0) * 480.0 / 640.0);
  EXPECT_NEAR(vertical_fov, expected, 1e-12);
  EXPECT_LT(vertical_fov, detail::kHorizontalFovRad);
}

TEST(ObserveHumanGeometryTest, CorrectionIsContinuousAtDeadbandAndDampedNearCenter) {
  constexpr double deadband = 0.04;
  const auto at_boundary = detail::BoundedFollowCorrection(
      deadband, detail::kHorizontalFovRad, deadband, 0.30, 0.035);
  const auto just_outside = detail::BoundedFollowCorrection(
      deadband + 1e-6, detail::kHorizontalFovRad, deadband, 0.30, 0.035);
  const auto at_full_gain = detail::BoundedFollowCorrection(
      0.30, detail::kHorizontalFovRad, deadband, 0.30, 0.035);
  EXPECT_DOUBLE_EQ(at_boundary.effective_angle_rad, 0.0);
  EXPECT_LT(std::abs(just_outside.effective_angle_rad), 1e-6);
  EXPECT_NEAR(just_outside.gain, 0.5, 1e-5);
  EXPECT_DOUBLE_EQ(at_full_gain.gain, 1.0);
  EXPECT_TRUE(at_full_gain.capped);
  EXPECT_NEAR(at_full_gain.effective_angle_rad, 0.035, 1e-12);
}

TEST(ObserveHumanGeometryTest, CorrectionPreservesSignsAndCapsBothAxes) {
  const auto left = detail::BoundedFollowCorrection(
      -0.20, detail::kHorizontalFovRad, 0.04, 0.30, 0.035);
  const auto right = detail::BoundedFollowCorrection(
      0.20, detail::kHorizontalFovRad, 0.04, 0.30, 0.035);
  EXPECT_NEAR(left.effective_angle_rad, -right.effective_angle_rad, 1e-12);
  EXPECT_LE(std::abs(left.effective_angle_rad), 0.035);
  const double vertical_fov = detail::VerticalFovRad(detail::kHorizontalFovRad, 640, 480);
  const auto tilt = detail::BoundedFollowCorrection(0.20, vertical_fov, 0.06, 0.30, 0.025);
  EXPECT_LE(std::abs(tilt.effective_angle_rad), 0.025);
}

TEST(ObserveHumanGeometryTest, CenteredHysteresisRequiresTwoExitSamples) {
  detail::CenteredState state{};
  state = detail::UpdateCenteredState(state, 0.03, 0.05, 0.04, 0.06, 0.08, 0.10);
  ASSERT_TRUE(state.centered);
  state = detail::UpdateCenteredState(state, 0.09, 0.0, 0.04, 0.06, 0.08, 0.10);
  EXPECT_TRUE(state.centered);
  EXPECT_EQ(state.consecutive_exit_samples, 1u);
  state = detail::UpdateCenteredState(state, 0.07, 0.0, 0.04, 0.06, 0.08, 0.10);
  EXPECT_TRUE(state.centered);
  EXPECT_EQ(state.consecutive_exit_samples, 0u);
  state = detail::UpdateCenteredState(state, 0.09, 0.0, 0.04, 0.06, 0.08, 0.10);
  state = detail::UpdateCenteredState(state, 0.09, 0.0, 0.04, 0.06, 0.08, 0.10);
  EXPECT_FALSE(state.centered);
}

nlohmann::json GimbalState(double pan_rate = 0.12, double tilt_rate = -0.05) {
  return {{"device_class", "gimbal"},
          {"state", "active"},
          {"health", "ready"},
          {"pan_rad", 0.1},
          {"tilt_rad", -0.1},
          {"pan_rate_rad_s", pan_rate},
          {"tilt_rate_rad_s", tilt_rate}};
}

TEST_F(ObserveHumanTest, TracksStableHumanWithPanAndTiltUntilCancelled) {
  std::atomic<std::size_t> reads{0};
  std::atomic<std::size_t> follow_calls{0};
  std::atomic<std::size_t> scan_calls{0};
  std::atomic<bool> followed{false};
  std::atomic<bool> read_while_following{false};
  std::mutex mutex;
  std::vector<double> pan_deltas;
  std::vector<double> tilt_deltas;
  std::vector<nlohmann::json> public_states;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json arguments, const std::string&, Cancelled) {
    if (arguments.value("stream", false)) {
      const std::size_t call = ++follow_calls;
      EXPECT_LT(arguments.value("pan_rad", 0.0), 0.0);
      EXPECT_EQ(arguments.value("reference", ""), "absolute");
      EXPECT_DOUBLE_EQ(arguments.value("speed_rad_s", 0.0), 0.4);
      {
        std::lock_guard lock(mutex);
        pan_deltas.push_back(arguments.value("pan_rad", 0.0));
        tilt_deltas.push_back(arguments.value("tilt_rad", 0.0));
      }
      if (call == 1) {
        const std::size_t reads_at_start = reads.load();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (reads.load() == reads_at_start && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        read_while_following = reads.load() > reads_at_start;
      }
      followed = true;
    } else {
      ++scan_calls;
    }
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::size_t read = ++reads;
    const std::uint64_t generation = read < 3 ? 1 : read < 5 ? 2 : std::min(read - 2, 8UL);
    const std::string id = read < 6 ? "human:" + std::to_string(read) : "human:owner";
    const nlohmann::json occluded_bbox =
        read <= 6 ? nlohmann::json({0, 0, 640, 300}) : nlohmann::json({0, 0, 10, 10});
    const nlohmann::json tracked_bbox = read <= 6 ? nlohmann::json({300, 470, 620, 479})
                                                  : nlohmann::json({300, 100, 620, 200});
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", read},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation * 100)},
                          {"source_gimbal", {{"pan_rad", 0.2},
                                             {"tilt_rad", -0.2},
                                             {"pan_rate_rad_s", 0.0},
                                             {"tilt_rate_rad_s", 0.0}}},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                           {"detections", {{{"id", "human:occluded"},
                                             {"kind", "human"},
                                             {"confidence", 0.99},
                                             {"attributes", {{"name", "unknown"}}},
                                             {"bbox", occluded_bbox}},
                                           {{"id", id},
                                             {"kind", "human"},
                                             {"confidence", 0.95},
                                             {"attributes",
                                              {{"name", read <= 6 ? "Alice" : "unknown"},
                                               {"name_source_generation", generation},
                                               {"name_similarity", read <= 6 ? 0.91 : 0.0}}},
                                             {"bbox", tracked_bbox}}}}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return reads.load() > 25; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    std::lock_guard lock(mutex);
    public_states.push_back(std::move(state));
    return true;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0}}, "observe-v2", hooks);

  ASSERT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(followed.load());
  EXPECT_TRUE(read_while_following.load());
  EXPECT_EQ(scan_calls.load(), 0u);
  EXPECT_GE(follow_calls.load(), 3u);
  EXPECT_GT(reads.load(), 5u);
  std::lock_guard lock(mutex);
  ASSERT_FALSE(tilt_deltas.empty());
  ASSERT_FALSE(pan_deltas.empty());
  EXPECT_NEAR(pan_deltas.front(),
              0.2 - (detail::NormalizedImageErrorToAngle(0.4375, detail::kHorizontalFovRad) -
                     detail::NormalizedImageErrorToAngle(0.04, detail::kHorizontalFovRad)),
              1e-12);
  const double initial_vertical_error =
      (2.0 * (470.0 + (479.0 - 470.0) * 0.42) - 480.0) / 480.0;
  const double upper_half_vertical_error =
      (2.0 * (100.0 + (200.0 - 100.0) * 0.42) - 480.0) / 480.0;
  const double filtered_vertical_error =
      0.8 * upper_half_vertical_error + 0.2 * initial_vertical_error;
  const double vertical_fov = detail::VerticalFovRad(detail::kHorizontalFovRad, 640, 480);
  const auto expected_tilt_correction = detail::BoundedFollowCorrection(
      filtered_vertical_error, vertical_fov, 0.06, 0.30, 0.025);
  EXPECT_NEAR(tilt_deltas.front(), -0.2 - expected_tilt_correction.effective_angle_rad, 1e-12);
  for (double target : pan_deltas) EXPECT_NEAR(target, pan_deltas.front(), 1e-12);
  EXPECT_TRUE(std::any_of(std::next(tilt_deltas.begin()), tilt_deltas.end(), [&](double target) {
    return target > tilt_deltas.front();
  }));
  for (double target : tilt_deltas) EXPECT_LE(target, -0.1);
  EXPECT_TRUE(std::any_of(tilt_deltas.begin(), tilt_deltas.end(), [](double target) {
    return std::abs(target + 0.1) < 1e-12;
  }));
  ASSERT_FALSE(public_states.empty());
  EXPECT_EQ(public_states.front().at("phase"), "scan");
  EXPECT_TRUE(std::any_of(public_states.begin(), public_states.end(), [](const auto& state) {
    return state.value("phase", "") == "confirm";
  }));
  const auto final_public = public_states.back();
  EXPECT_EQ(final_public.at("phase"), "track");
  EXPECT_FALSE(final_public.contains("pose_analysis"));
  EXPECT_FALSE(final_public.contains("pose"));
  EXPECT_FALSE(final_public.contains("action"));
  EXPECT_DOUBLE_EQ(final_public.at("pan_rate_rad_s"), 0.12);
  EXPECT_DOUBLE_EQ(final_public.at("tilt_rate_rad_s"), -0.05);
  EXPECT_DOUBLE_EQ(final_public.at("speed_rad_s"), 0.13);
  for (std::size_t index = 1; index < public_states.size(); ++index)
    EXPECT_NE(public_states[index], public_states[index - 1]);
  for (const auto& state : public_states) {
    EXPECT_FALSE(state.contains("identity"));
    EXPECT_FALSE(state.contains("data"));
    EXPECT_EQ(state.dump().find("raw-media-must-not-be-published"), std::string::npos);
  }
}

TEST_F(ObserveHumanTest, UsesLatePublicationsWithoutCountingOneFrameTwice) {
  std::atomic<int> reads{0};
  std::atomic<int> follows{0};
  std::atomic<int> scans{0};
  std::atomic<int> tracked_at_read{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json arguments, const std::string&, Cancelled) {
    if (arguments.value("stream", false)) ++follows;
    else ++scans;
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const int read = ++reads;
    const std::uint64_t generation = read == 1 ? 1 : read <= 5 ? 2 :
                                      read <= 8 ? 3 : read == 9 ? 4 : 5;
    const bool pending = read == 2;
    const bool detected = read >= 3;
    const bool named = read == 2 || read >= 4;
    const std::string name = read == 7 ? "Bob" : "Alice";
    nlohmann::json detections = nlohmann::json::array();
    if (read == 2 || detected) {
      detections.push_back({{"id", "human:7"}, {"kind", "human"},
                            {"confidence", 0.95}, {"bbox", {300, 280, 620, 470}},
                            {"attributes", {{"name", named ? nlohmann::json(name) : nlohmann::json(nullptr)},
                                            {"name_source_generation", generation},
                                            {"action", read >= 5 ? "fallen" : "unknown"}}}});
    }
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"image_width_px", 640}, {"image_height_px", 480},
                          {"models", {{"human", {{"status", pending ? "pending" : "complete"},
                                                   {"source_generation", pending ? generation - 1 : generation}}}}},
                          {"detections", std::move(detections)}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return reads.load() >= 15; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") tracked_at_read = reads.load();
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0}}, "late-publications", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(tracked_at_read.load(), 9);
  EXPECT_EQ(scans.load(), 0);
  EXPECT_LE(follows.load(), 2);
  EXPECT_GE(follows.load(), 1);
}

TEST_F(ObserveHumanTest, ReacquiresAfterThreeDistinctLateDetectionGenerations) {
  std::atomic<int> reads{0};
  std::atomic<bool> saw_initial_track{false};
  std::atomic<bool> saw_reacquire{false};
  std::atomic<int> retracked_at_read{0};
  std::atomic<int> resumed_scan_moves{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json arguments, const std::string&, Cancelled) {
    if (saw_reacquire.load() && !arguments.value("stream", false)) ++resumed_scan_moves;
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const int read = ++reads;
    const std::uint64_t generation = read <= 6 ? read : read <= 10 ? 7 : read == 11 ? 8 : 9;
    const bool pending = read == 7;
    nlohmann::json detections = nlohmann::json::array();
    if ((read >= 2 && read <= 4) || read == 7 || read >= 9) {
      detections.push_back({{"id", read <= 4 ? "human:old" : "human:new"},
                            {"kind", "human"}, {"confidence", 0.95},
                            {"bbox", {300, 280, 620, 470}},
                            {"attributes", {{"name", "Alice"},
                                            {"name_source_generation", generation}}}});
    }
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"image_width_px", 640}, {"image_height_px", 480},
                          {"models", {{"human", {{"status", pending ? "pending" : "complete"},
                                                   {"source_generation", pending ? generation - 1 : generation}}}}},
                          {"detections", std::move(detections)}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return retracked_at_read.load() != 0 || reads.load() >= 20; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") saw_initial_track = true;
    if (state.value("phase", "") == "scan" && saw_initial_track.load()) saw_reacquire = true;
    if (state.value("phase", "") == "track" && saw_reacquire.load())
      retracked_at_read = reads.load();
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"target_loss_timeout_ms", 1}}, "late-reacquire", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(retracked_at_read.load(), 12);
  EXPECT_GE(resumed_scan_moves.load(), 1);
}

TEST_F(ObserveHumanTest, DurationAlsoEndsTrackingAfterAConfirmedHuman) {
  std::atomic<std::uint64_t> reads{0};
  std::atomic<bool> tracked{false};
  std::atomic<int> absence_facts{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto generation = ++reads;
    nlohmann::json detections = nlohmann::json::array();
    if (generation > 1) {
      detections.push_back({{"id", "human:7"}, {"kind", "human"},
                            {"confidence", 0.95}, {"bbox", {300, 280, 620, 470}},
                            {"attributes", {{"name", "Alice"},
                                            {"name_source_generation", generation}}}});
    }
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"image_width_px", 640}, {"image_height_px", 480},
                          {"detections", std::move(detections)}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [] { return false; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") tracked = true;
    return true;
  };
  hooks.emit_signal = [&](const Signal&) { ++absence_facts; return true; };
  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"acquire_timeout_ms", 250}}, "tracking-duration", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(tracked.load());
  EXPECT_EQ(result.result.at("found"), true);
  EXPECT_EQ(absence_facts.load(), 0);
}

TEST_F(ObserveHumanTest, DurationExpiresBeforeFirstCameraFrame) {
  std::atomic<int> facts{0};
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [](const std::string&) { return nlohmann::json::object(); };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [] { return false; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [](nlohmann::json) { return true; };
  hooks.emit_signal = [&](const Signal&) { ++facts; return true; };
  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"acquire_timeout_ms", 10}}, "no-first-frame", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(result.result.at("found").get<bool>());
  EXPECT_EQ(facts.load(), 1);
}

TEST_F(ObserveHumanTest, DeclaresBoundedPublicStateWithoutCrossSkillOutput) {
  Registry registry(timer_);
  RegisterObserveHuman(registry);
  const auto* skill = registry.Find("observe_human_legacy", "1.0.0");
  ASSERT_NE(skill, nullptr);
  ASSERT_TRUE(skill->execution_state_schema.has_value());
  EXPECT_FALSE(skill->related_process_state_schema.has_value());
  EXPECT_FALSE(skill->execution_state_schema->at("properties").contains("identity"));
  EXPECT_EQ(skill->execution_state_schema->at("properties").at("target_id").at("maxLength"),
            128);
  EXPECT_FALSE(skill->execution_state_schema->at("properties").contains("pose_analysis"));
  EXPECT_FALSE(skill->execution_state_schema->at("properties").contains("pose"));
  EXPECT_FALSE(skill->execution_state_schema->at("properties").contains("action"));
  EXPECT_TRUE(skill->inference_authorities.empty());
  EXPECT_TRUE(skill->execution_state_schema->at("properties").contains("pan_rate_rad_s"));
  EXPECT_TRUE(skill->execution_state_schema->at("properties").contains("tilt_rate_rad_s"));
  EXPECT_TRUE(skill->execution_state_schema->at("properties").contains("speed_rad_s"));
  EXPECT_TRUE(skill->parameters_schema.at("properties").contains("acquire_timeout_ms"));
  EXPECT_TRUE(skill->returns_schema.at("properties").contains("found"));
  ASSERT_EQ(skill->emitted_signals.size(), 1u);
  EXPECT_EQ(skill->emitted_signals.front().name, "state.fact");
  ASSERT_EQ(skill->resources.size(), 2u);
  EXPECT_EQ(skill->resources[1].uri_template,
            "robots/{robot_name}/devices/{gimbal_device_id}/state");
}

TEST_F(ObserveHumanTest, DoesNotRunPrivatePerceptionInference) {
  std::atomic<std::size_t> reads{0};
  std::atomic<std::size_t> completed{0};
  std::atomic<std::size_t> related_publications{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "human:7"},
                                            {"kind", "human"},
                                            {"confidence", 0.95},
                                            {"bbox", {220, 200, 420, 460}}}}}};
  };
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    ++completed;
    nlohmann::json value = nlohmann::json::object();
    if (request.authority == InferenceAuthority::kFaceRecognition) {
      value = {{"status", "success"},
               {"operation", "recognize"},
               {"data", {{"faces", {{{"identity", "Alice"},
                                         {"known", true},
                                         {"similarity", 0.91},
                                         {"bbox", {250, 120, 290, 180}}},
                                        {{"identity", "Bob"},
                                         {"known", true},
                                         {"similarity", 0.92},
                                         {"bbox", {330, 120, 370, 180}}}}},
                          {"face_count", 2}}}};
    }
    return InferenceResult{request.authority, request.source, request.source_sequence,
                           request.source_generation, std::move(value)};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  hooks.cancelled = [&] { return reads.load() > 10 || std::chrono::steady_clock::now() >= deadline; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [](nlohmann::json) { return true; };
  hooks.publish_related_process_state = [&](nlohmann::json) {
    ++related_publications;
    return true;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0}},
      "ambiguous-face", hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(completed.load(), 0u);
  EXPECT_EQ(related_publications.load(), 0u);
}

TEST_F(ObserveHumanTest, StopsAndDrainsActiveScanOnlyAfterStableConfirmation) {
  std::atomic<std::size_t> reads{0};
  std::atomic<std::size_t> inference_calls{0};
  std::atomic<std::size_t> scan_cancellations{0};
  std::atomic<std::size_t> positioning_calls{0};
  std::atomic<bool> scan_started{false};
  std::atomic<bool> scan_cancel_requested{false};
  std::atomic<bool> scan_terminal{false};
  std::atomic<bool> inference_started_after_scan{true};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json arguments, const std::string&,
                                       Cancelled) {
    if (arguments.value("stream", false)) {
      return nlohmann::json{{"accepted", true},
                            {"reason", nullptr},
                            {"result", {{"status", "succeeded"}}}};
    }
    if (++positioning_calls <= 2) {
      return nlohmann::json{{"accepted", true},
                            {"reason", nullptr},
                            {"result", {{"status", "succeeded"}}}};
    }
    scan_started = true;
    while (!scan_cancel_requested.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    scan_terminal = true;
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "cancelled"}}}};
  };
  capabilities.cancel_primitive = [&](const std::string&) {
    if (!scan_terminal.load()) ++scan_cancellations;
    scan_cancel_requested = true;
  };
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    nlohmann::json detections = nlohmann::json::array();
    if (scan_started.load() && generation != 3) {
      detections.push_back({{"id", "human:7"},
                            {"kind", "human"},
                            {"confidence", 0.95},
                            {"attributes", {{"name", "Alice"},
                                            {"name_source_generation",
                                             generation <= 4 ? 1 : generation},
                                            {"name_similarity", 0.91}}},
                            {"bbox", {220, 200, 420, 460}}});
    }
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", std::move(detections)}};
  };
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    inference_started_after_scan = inference_started_after_scan.load() && scan_terminal.load();
    ++inference_calls;
    return InferenceResult{request.authority, request.source, request.source_sequence,
                           request.source_generation, nlohmann::json::object()};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return scan_terminal.load() && reads.load() >= 8; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0}},
      "immediate-stop", hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(scan_cancellations.load(), 1u);
  EXPECT_TRUE(scan_terminal.load());
  EXPECT_TRUE(inference_started_after_scan.load());
  EXPECT_EQ(inference_calls.load(), 0u);
  EXPECT_GE(reads.load(), 6u);
}

TEST_F(ObserveHumanTest, EmptyScanRepeatsSmoothAbsoluteWaypointPattern) {
  std::atomic<std::size_t> reads{0};
  std::atomic<std::size_t> inference_calls{0};
  std::mutex mutex;
  std::vector<nlohmann::json> primitive_arguments;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                       nlohmann::json arguments, const std::string&, Cancelled) {
    std::lock_guard lock(mutex);
    if (arguments.value("stream", false)) {
      return nlohmann::json{{"accepted", true},
                            {"reason", nullptr},
                            {"result", {{"status", "succeeded"}}}};
    }
    primitive_arguments.push_back(std::move(arguments));
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", nlohmann::json::array()}};
  };
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    ++inference_calls;
    return InferenceResult{request.authority, request.source, request.source_sequence,
                           request.source_generation, nlohmann::json::object()};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] {
    std::lock_guard lock(mutex);
    return primitive_arguments.size() >= 8;
  };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0}},
      "repeating-scan", hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(inference_calls.load(), 0u);
  std::lock_guard lock(mutex);
  ASSERT_GE(primitive_arguments.size(), 8u);
  EXPECT_NEAR(primitive_arguments[0].value("pan_rad", 0.0), 0.1, 1e-12);
  EXPECT_NEAR(primitive_arguments[0].value("tilt_rad", 0.0), -0.4, 1e-12);
  EXPECT_NEAR(primitive_arguments[1].value("pan_rad", 0.0), 0.1, 1e-12);
  EXPECT_NEAR(primitive_arguments[1].value("tilt_rad", 0.0), -0.1, 1e-12);
  const double pi = 3.14159265358979323846;
  const double expected[] = {0.1 - pi / 4, 0.1, 0.1 + pi / 4, 0.1};
  for (std::size_t i = 0; i < primitive_arguments.size(); ++i) {
    EXPECT_EQ(primitive_arguments[i].value("reference", ""), "absolute");
    EXPECT_FALSE(primitive_arguments[i].value("stream", true));
    EXPECT_DOUBLE_EQ(primitive_arguments[i].value("speed_rad_s", 0.0), 0.1);
  }
  for (std::size_t i = 2; i < primitive_arguments.size(); ++i) {
    EXPECT_NEAR(primitive_arguments[i].value("pan_rad", 0.0), expected[(i - 2) % 4], 1e-12);
    EXPECT_NEAR(primitive_arguments[i].value("tilt_rad", 0.0), -0.1, 1e-12);
  }
}

TEST_F(ObserveHumanTest, TracksStableUnidentifiedFallenHuman) {
  std::atomic<std::uint64_t> reads{0};
  std::mutex mutex;
  std::vector<nlohmann::json> public_states;
  std::vector<nlohmann::json> positioning_arguments;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [&](const PrimitiveReference&, const std::string&,
                                        nlohmann::json arguments, const std::string&, Cancelled) {
    if (!arguments.value("stream", false)) {
      std::lock_guard lock(mutex);
      positioning_arguments.push_back(std::move(arguments));
    }
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    nlohmann::json detections = nlohmann::json::array();
    if (generation > 1) {
      detections.push_back({{"id", "human:fallen"},
                            {"kind", "human"},
                             {"confidence", 0.4},
                             {"bbox", {220, 120, 420, 460}},
                            {"attributes", {{"pose", "lie"}, {"action", "fallen"}}}});
    }
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", std::move(detections)}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] {
    std::lock_guard lock(mutex);
    return std::any_of(public_states.begin(), public_states.end(), [](const auto& state) {
      return state.value("phase", "") == "track";
    });
  };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    std::lock_guard lock(mutex);
    public_states.push_back(std::move(state));
    return true;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0}},
      "fallen-without-name", hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  std::lock_guard lock(mutex);
  const auto tracked = std::find_if(public_states.begin(), public_states.end(), [](const auto& state) {
    return state.value("phase", "") == "track";
  });
  ASSERT_NE(tracked, public_states.end());
  EXPECT_EQ(tracked->at("target_id"), "human:fallen");
  ASSERT_EQ(positioning_arguments.size(), 1u);
  EXPECT_NEAR(positioning_arguments.front().value("pan_rad", 0.0), 0.1, 1e-12);
  EXPECT_NEAR(positioning_arguments.front().value("tilt_rad", 0.0), -0.4, 1e-12);
}

TEST_F(ObserveHumanTest, DoesNotTreatOtherUnidentifiedActionsAsOwner) {
  std::atomic<std::uint64_t> reads{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    return nlohmann::json{
        {"source", "camera.front"},
        {"source_sequence", generation},
        {"source_generation", generation},
        {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
        {"image_width_px", 640},
        {"image_height_px", 480},
        {"detections",
         {{{"id", "human:waving"},
           {"kind", "human"},
           {"confidence", 0.95},
           {"bbox", {220, 120, 420, 460}},
           {"attributes", {{"pose", "stand"}, {"action", "waving"}}}}}}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);

  ExecutionHooks hooks;
  hooks.emit_signal = [](const Signal&) { return true; };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0},
       {"acquire_timeout_ms", 10}},
      "waving-without-name", hooks);

  ASSERT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(result.result.at("found").get<bool>());
}

TEST_F(ObserveHumanTest, EmptyAcquisitionCompletesAndEmitsOwnerAbsentFact) {
  std::atomic<std::uint64_t> reads{0};
  std::mutex mutex;
  std::vector<Signal> emitted_signals;
  std::vector<nlohmann::json> public_states;
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", static_cast<std::int64_t>(generation)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", nlohmann::json::array()}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [] { return false; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.emit_signal = [&](const Signal& signal) {
    std::lock_guard lock(mutex);
    emitted_signals.push_back(signal);
    return true;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    std::lock_guard lock(mutex);
    public_states.push_back(std::move(state));
    return true;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"acquire_timeout_ms", 1}},
      "empty-timeout", hooks);

  ASSERT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result, nlohmann::json({{"found", false}}));
  std::lock_guard lock(mutex);
  ASSERT_EQ(emitted_signals.size(), 1u);
  EXPECT_EQ(emitted_signals.front().name, "state.fact");
  EXPECT_EQ(emitted_signals.front().payload,
            nlohmann::json({{"key", "owner.found"}, {"value", false}, {"sequence", 1}}));
  ASSERT_FALSE(public_states.empty());
  EXPECT_EQ(public_states.back().at("phase"), "absent");
}

TEST_F(ObserveHumanTest, CancellationDoesNotEmitOwnerAbsentFact) {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<int> cancellation_checks{0};
  std::atomic<int> emitted_signals{0};
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t next = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", next},
                          {"source_generation", next},
                          {"source_timestamp_ns", static_cast<std::int64_t>(next)},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", nlohmann::json::array()}};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return ++cancellation_checks >= 2; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.emit_signal = [&](const Signal&) {
    ++emitted_signals;
    return true;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"},
       {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"acquire_timeout_ms", 30000}},
      "cancel-before-timeout", hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_EQ(emitted_signals.load(), 0);
}

TEST_F(ObserveHumanTest, CancelsAfterStableConfirmationWithoutInference) {
  std::atomic<std::size_t> reads{0};
  std::atomic<std::size_t> inference_calls{0};
  std::atomic<std::size_t> stable_cancellation_checks{0};
  RuntimeCapabilities capabilities;
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true},
                          {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const std::uint64_t generation = ++reads;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", generation},
                          {"source_generation", generation},
                          {"source_timestamp_ns", 1},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                           {"detections", {{{"id", "human:7"},
                                             {"kind", "human"},
                                             {"confidence", 0.95},
                                             {"attributes", {{"name", "Alice"},
                                                             {"name_source_generation", generation},
                                                             {"name_similarity", 0.91}}},
                                             {"bbox", {220, 100, 420, 460}}}}}};
  };
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    ++inference_calls;
    return InferenceResult{request.authority, request.source, request.source_sequence,
                           request.source_generation, nlohmann::json::object()};
  };
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] {
    return reads.load() >= 3 && ++stable_cancellation_checks >= 2;
  };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };

  const auto result = registry.ExecuteWithHooks(
      "observe_human_legacy", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0}}, "cancel-before-infer",
      hooks);

  EXPECT_EQ(result.status, RunResult::Status::kCancelled)
      << result.reason_code << ": " << result.message;
  EXPECT_EQ(stable_cancellation_checks.load(), 2u);
  EXPECT_EQ(inference_calls.load(), 0u);
}

TEST_F(ObserveHumanTest, VersionTwoAcceptsNamedFaceBetweenThirteenTwentyFourthsAndTwoThirds) {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> tracking{false};
  std::string selected_id;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto frame = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", frame},
                          {"source_generation", frame},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "upper"}, {"kind", "human"},
                                           {"confidence", 0.99},
                                           {"attributes", {{"name", "Upper"},
                                                           {"name_source_generation", frame}}},
                                           {"bbox", {0, 0, 640, 300}}},
                                          {{"id", "anonymous"}, {"kind", "human"},
                                           {"confidence", 0.95},
                                           {"attributes", {{"name", nullptr}}},
                                           {"bbox", {0, 320, 640, 479}}},
                                          {{"id", "named"}, {"kind", "human"},
                                            {"confidence", 0.45},
                                           {"attributes", {{"name", "Alice"},
                                                           {"name_source_generation", frame}}},
                                           {"bbox", {300, 200, 500, 340}}}}}};
  };
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ASSERT_NE(registry.Find("observe_human_legacy", "1.0.0"), nullptr);
  ASSERT_NE(registry.Find("observe_human", "1.0.0"), nullptr);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return tracking.load() || generation.load() > 15; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") {
      selected_id = state.value("target_id", "");
      tracking = true;
    }
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 1000}}, "v2-named-priority", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(tracking.load());
  EXPECT_EQ(selected_id, "named");
}

TEST_F(ObserveHumanTest, VersionTwoPrioritizesLyingHumanOverNamedFallenHuman) {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> tracking{false};
  std::string selected_id;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto frame = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", frame},
                          {"source_generation", frame},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "named"}, {"kind", "human"},
                                            {"confidence", 0.99},
                                            {"attributes", {{"name", "Alice"},
                                                            {"name_source_generation", frame},
                                                            {"pose", "stand"},
                                                            {"action", "fallen"}}},
                                            {"bbox", {20, 180, 620, 470}}},
                                           {{"id", "lying"}, {"kind", "human"},
                                            {"confidence", 0.8},
                                            {"attributes", {{"name", "unknown"},
                                                            {"pose", "lie"},
                                                            {"action", "unknown"}}},
                                           {"bbox", {260, 280, 380, 470}}}}}};
  };
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return tracking.load() || generation.load() > 15; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") {
      selected_id = state.value("target_id", "");
      tracking = true;
    }
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 1000}}, "v2-lying-priority", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(tracking.load());
  EXPECT_EQ(selected_id, "lying");
}

TEST_F(ObserveHumanTest, VersionTwoOnlyFollowsUnnamedPeopleWhenLying) {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> tracking{false};
  std::string selected_id;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto frame = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", frame},
                          {"source_generation", frame},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "unknown"}, {"kind", "human"},
                                           {"confidence", 0.9},
                                            {"attributes", {{"name", "unknown"}, {"pose", "stand"}}},
                                           {"bbox", {0, 320, 500, 479}}},
                                          {{"id", "no-face"}, {"kind", "human"},
                                            {"confidence", 0.45},
                                            {"attributes", {{"name", nullptr}, {"pose", "lie"}}},
                                           {"bbox", {350, 340, 600, 479}}}}}};
  };
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"reason", nullptr},
                          {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return tracking.load() || generation.load() > 15; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") {
      selected_id = state.value("target_id", "");
      tracking = true;
    }
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 1000}}, "v2-unknown-exclusion", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(tracking.load());
  EXPECT_EQ(selected_id, "no-face");
}

TEST_F(ObserveHumanTest, VersionTwoDoesNotFollowUnnamedFallenWithoutLyingPose) {
  std::atomic<std::uint64_t> generation{0};
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto frame = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", frame},
                          {"source_generation", frame},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "unnamed-fallen"}, {"kind", "human"},
                                           {"confidence", 0.95},
                                           {"attributes", {{"name", nullptr},
                                                           {"pose", "sit"},
                                                           {"action", "fallen"}}},
                                           {"bbox", {250, 280, 450, 470}}}}}};
  };
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.emit_signal = [](const Signal&) { return true; };
  const auto result = registry.ExecuteWithHooks(
      "observe_human", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 0}, {"acquire_timeout_ms", 80}},
      "v2-fallen-without-lie", hooks);

  ASSERT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_FALSE(result.result.at("found").get<bool>());
}

TEST_F(ObserveHumanTest, VersionTwoKeepsFirstEligiblePersonRegardlessOfBboxArea) {
  std::atomic<std::uint64_t> generation{0};
  std::atomic<bool> tracking{false};
  std::string selected_id;
  RuntimeCapabilities capabilities;
  capabilities.read_resource = [&](const std::string& uri) {
    if (uri.find("/devices/") != std::string::npos) return GimbalState();
    const auto frame = ++generation;
    return nlohmann::json{{"source", "camera.front"},
                          {"source_sequence", frame},
                          {"source_generation", frame},
                          {"image_width_px", 640},
                          {"image_height_px", 480},
                          {"detections", {{{"id", "first"}, {"kind", "human"},
                                           {"confidence", 0.8},
                                           {"attributes", {{"pose", "lie"}}},
                                           {"bbox", {300, 280, 400, 470}}},
                                          {{"id", "larger"}, {"kind", "human"},
                                           {"confidence", 0.9},
                                           {"attributes", {{"pose", "lie"}}},
                                           {"bbox", {10, 200, 620, 470}}}}}};
  };
  capabilities.execute_primitive = [](const PrimitiveReference&, const std::string&,
                                      nlohmann::json, const std::string&, Cancelled) {
    return nlohmann::json{{"accepted", true}, {"result", {{"status", "succeeded"}}}};
  };
  capabilities.cancel_primitive = [](const std::string&) {};
  Registry registry(timer_, std::move(capabilities));
  RegisterObserveHuman(registry);
  ExecutionHooks hooks;
  hooks.cancelled = [&] { return tracking.load() || generation.load() > 15; };
  hooks.receive_signal = [](std::chrono::milliseconds) -> std::optional<Signal> {
    return std::nullopt;
  };
  hooks.publish_execution_state = [&](nlohmann::json state) {
    if (state.value("phase", "") == "track") {
      selected_id = state.value("target_id", "");
      tracking = true;
    }
    return true;
  };
  const auto result = registry.ExecuteWithHooks(
      "observe_human", "1.0.0",
      {{"robot_name", "robot"}, {"gimbal_device_id", "gimbal-0"}, {"reacquire_tilt_rad", 0.0},
       {"observation_hold_ms", 1000}, {"acquire_timeout_ms", 2000}},
      "v2-first-eligible", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCancelled);
  EXPECT_TRUE(tracking.load());
  EXPECT_EQ(selected_id, "first");
}

}  // namespace
}  // namespace island::skills::native

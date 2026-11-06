#include "vendor_base.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 1s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

class FakeDriver : public eos::backends::vendor::VendorBaseDriver {
 public:
  void Stand() override { ++stand; }
  void Walk() override { ++walk; }
  void Lie() override { ++lie; }
  eos::hardware::ReleaseResult NavigateRelative(double x_m, double y_m, double yaw_rad) override {
    ++navigate_relative;
    last_x_m = x_m;
    last_y_m = y_m;
    last_yaw_rad = yaw_rad;
    return eos::hardware::ReleaseResult::kAccepted;
  }
  void Clamp() override { ++clamp; }
  void Halt() override { ++halt; }
  void SetGait(const std::string& gait) override { last_gait = gait; }

  int stand{0};
  int walk{0};
  int lie{0};
  int navigate_relative{0};
  int clamp{0};
  int halt{0};
  double last_x_m{0.0};
  double last_y_m{0.0};
  double last_yaw_rad{0.0};
  std::string last_gait;
};

TEST(VendorBaseTest, ObstacleReadKeepsUnknownSectorsAndMapsFailures) {
  class ObstacleDriver final : public FakeDriver {
   public:
    bool SupportsObstacles() const override { return true; }
    eos::hardware::ObstacleSnapshot ReadObstacles() const override {
      if (fail) throw std::runtime_error("collector unavailable");
      eos::hardware::ObstacleSnapshot sample;
      sample.state = eos::hardware::ObstacleState::kFresh;
      sample.age_s = 0.2;
      sample.sectors[0].point_count = 2;
      sample.sectors[0].distance_m = 0.75;
      return sample;
    }
    mutable bool fail{false};
  };
  auto driver = std::make_unique<ObstacleDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  EXPECT_TRUE(backend.SupportsObstacleSnapshot());
  const auto fresh = backend.ReadObstacleSnapshot();
  ASSERT_TRUE(fresh);
  EXPECT_EQ(fresh->state, eos::hardware::ObstacleState::kFresh);
  EXPECT_EQ(fresh->sectors[0].point_count, 2u);
  ASSERT_TRUE(fresh->sectors[0].distance_m);
  EXPECT_DOUBLE_EQ(*fresh->sectors[0].distance_m, 0.75);
  EXPECT_FALSE(fresh->sectors[1].distance_m);
  observed->fail = true;
  const auto failed = backend.ReadObstacleSnapshot();
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->state, eos::hardware::ObstacleState::kTransportError);
  for (const auto& sector : failed->sectors) {
    EXPECT_EQ(sector.point_count, 0u);
    EXPECT_FALSE(sector.distance_m);
  }
}

TEST(VendorBaseTest, ObstacleReadUnsupportedByDefault) {
  eos::backends::vendor::VendorBase backend("base-0", std::make_unique<FakeDriver>());
  EXPECT_FALSE(backend.SupportsObstacleSnapshot());
  EXPECT_FALSE(backend.ReadObstacleSnapshot());
}

TEST(VendorBaseTest, StandaloneGaitUsesDriverWithoutNavigation) {
  auto driver = std::make_unique<FakeDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  EXPECT_EQ(backend.ExecuteGait({"quiet", "gait-1"}), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->last_gait, "quiet");
  EXPECT_EQ(observed->navigate_relative, 0);
}

TEST(VendorBaseTest, NavigationDoesNotBlockHealthReads) {
  class BlockingNavigationDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateRelative(double, double, double) override {
      entered.store(true);
      while (!released.load()) std::this_thread::sleep_for(1ms);
      return eos::hardware::ReleaseResult::kAccepted;
    }

    std::atomic_bool entered{false};
    std::atomic_bool released{false};
  };

  auto driver = std::make_unique<BlockingNavigationDriver>();
  BlockingNavigationDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  const eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
                                                              std::chrono::steady_clock::now()};
  auto result =
      std::async(std::launch::async, [&] { return backend.ExecuteNavigateRelative(navigation); });
  ASSERT_TRUE(WaitFor([&] { return observed->entered.load(); }));

  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms);

  observed->released.store(true);
  EXPECT_EQ(result.get(), eos::hardware::ReleaseResult::kAccepted);
}

TEST(VendorBaseTest, MapsPosturesAndDeclaresStatusFeed) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));

  EXPECT_EQ(backend.Identify().name, "vendor-base");
  EXPECT_EQ(backend.Identify().kind, eos::hardware::DeviceKind::kBase);
  EXPECT_DOUBLE_EQ(backend.DeclareFeeds().proprioception.max_rate_hz, 2.0);
  EXPECT_DOUBLE_EQ(backend.DeclareFeeds().health.max_rate_hz, 10.0);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "stand"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kWalk, "walk"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kLie, "lie"}),
            eos::hardware::ReleaseResult::kAccepted);
  const eos::hardware::BaseNavigateRelativeCommand navigation{1.25, -0.5, 0.75,
                                                              std::chrono::steady_clock::now()};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  backend.Clamp();

  EXPECT_EQ(observed->stand, 1);
  EXPECT_EQ(observed->walk, 1);
  EXPECT_EQ(observed->lie, 1);
  EXPECT_EQ(observed->navigate_relative, 1);
  EXPECT_DOUBLE_EQ(observed->last_x_m, 1.25);
  EXPECT_DOUBLE_EQ(observed->last_y_m, -0.5);
  EXPECT_DOUBLE_EQ(observed->last_yaw_rad, 0.75);
  EXPECT_EQ(observed->clamp, 1);
}

TEST(VendorBaseTest, ForwardsPerceivedStairsGaitAndRejectsInvalidGaits) {
  class GaitDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateRelativeWithGait(double x, double y, double yaw,
                                                          int gait) override {
      last_gait = gait;
      return NavigateRelative(x, y, yaw);
    }
    int last_gait{-1};
  };
  auto driver = std::make_unique<GaitDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  eos::hardware::BaseNavigateRelativeCommand command{0.3, 0.0, 0.0,
                                                     std::chrono::steady_clock::now()};
  command.gait = 4;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->last_gait, 4);
  EXPECT_EQ(observed->navigate_relative, 1);

  command.gait = 3;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kRejected);
  command.gait = 4;
  command.stream = true;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(observed->navigate_relative, 1);
}

TEST(VendorBaseTest, AbsoluteNavigationForwardsMapTargetHeightAndSpeed) {
  class AbsoluteDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateAbsolute(double x, double y, double z,
                                                  double yaw, int speed) override {
      ++absolute_calls;
      last_target = {x, y, yaw};
      last_z = z;
      last_speed = speed;
      return eos::hardware::ReleaseResult::kAccepted;
    }
    eos::hardware::ReleaseResult NavigateAbsoluteWithGait(double x, double y, double z,
                                                          double yaw, int speed, int gait) override {
      last_gait = gait;
      return NavigateAbsolute(x, y, z, yaw, speed);
    }
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 13.0, .y_m = 11.0, .z_m = -0.53, .yaw_rad = -3.08,
          .map_resolution_m = 0.05, .location = 0};
    }
    int absolute_calls{0};
    std::array<double, 3> last_target{};
    double last_z{0.0};
    int last_speed{-1};
    int last_gait{-1};
  };

  auto driver = std::make_unique<AbsoluteDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 5ms);
  ASSERT_TRUE(WaitFor([&] {
    const auto pose = backend.ReadBaseTelemetry();
    return pose && pose->fresh && pose->localization_valid;
  }));
  eos::hardware::BaseNavigateRelativeCommand command{1.2, 0.0, 0.0,
                                                     std::chrono::steady_clock::now()};
  command.absolute = true;
  command.stream_map_target = std::array<double, 3>{14.525529, 10.965195, -3.0782821};
  command.z_m = -0.4;
  command.speed = 2;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->absolute_calls, 1);
  EXPECT_EQ(observed->navigate_relative, 0);
  EXPECT_EQ(observed->last_target, *command.stream_map_target);
  EXPECT_DOUBLE_EQ(observed->last_z, -0.4);
  EXPECT_EQ(observed->last_speed, 2);
  EXPECT_EQ(observed->last_gait, 0);

  command.z_m.reset();
  command.speed.reset();
  command.gait = 4;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_DOUBLE_EQ(observed->last_z, -0.53);
  EXPECT_EQ(observed->last_speed, 1);
  EXPECT_EQ(observed->last_gait, 4);

  command.speed = 1.5;
  EXPECT_EQ(backend.ExecuteNavigateRelative(command), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(observed->absolute_calls, 2);
}

TEST(VendorBaseTest, NavigationSettlementTimeoutDoesNotLatchBackend) {
  class SettlingDriver final : public FakeDriver {
   public:
    void Stand() override {
      if (++attempts == 1) throw eos::backends::vendor::NavigationSettlingTimeout();
      FakeDriver::Stand();
    }
    int attempts{0};
  };
  auto driver = std::make_unique<SettlingDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "first"}),
            eos::hardware::ReleaseResult::kRejected);
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "retry"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->stand, 1);
}

TEST(VendorBaseTest, PollsAndNormalizesType1002Status) {
  class StatusDriver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      ++polls;
      if (!enabled.load()) return std::nullopt;
      return eos::backends::vendor::VendorBaseStatus{
          .motion_state = 16,
          .x_m = 1.25,
          .y_m = -2.5,
          .z_m = 0.3,
          .roll_rad = 0.1,
          .pitch_rad = -0.2,
          .yaw_rad = 1.5,
          .speed_m_s = 0.45,
          .current_odometry_km = 1.25,
          .total_odometry_km = 100.5,
          .current_runtime = 12,
          .total_runtime = 345,
          .map_resolution_m = 0.05,
          .map_origin_x_m = -10.0,
          .map_origin_y_m = -20.0,
          .map_height_pixels = 1024.0,
          .battery_percent = 52,
          .location = 0,
          .rtk_state = 3,
          .dock_state = 4,
          .gait_state = 34,
          .motor_state = 6,
          .charge_state = 7,
          .control_mode = 1,
          .map_update_state = 9,
      };
    }

    std::atomic<bool> enabled{false};
    std::atomic<int> polls{0};
  };

  auto driver = std::make_unique<StatusDriver>();
  StatusDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 5ms);
  ASSERT_TRUE(WaitFor([&] { return observed->polls.load() > 0; }));
  EXPECT_FALSE(backend.ReadBaseTelemetry().has_value());

  observed->enabled = true;
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  const auto telemetry = *backend.ReadBaseTelemetry();
  EXPECT_TRUE(telemetry.valid);
  EXPECT_TRUE(telemetry.fresh);
  EXPECT_TRUE(telemetry.pose_valid);
  EXPECT_TRUE(telemetry.localization_valid);
  EXPECT_DOUBLE_EQ(telemetry.x_m, 1.25);
  EXPECT_DOUBLE_EQ(*telemetry.z_m, 0.3);
  EXPECT_DOUBLE_EQ(*telemetry.forward_speed_m_s, 0.45);
  EXPECT_DOUBLE_EQ(*telemetry.current_odometry_m, 1250.0);
  EXPECT_DOUBLE_EQ(*telemetry.total_odometry_m, 100500.0);
  EXPECT_EQ(*telemetry.battery_percent, 52);
  EXPECT_DOUBLE_EQ(*telemetry.map_resolution_m_per_pixel, 0.05);
  EXPECT_DOUBLE_EQ(*telemetry.map_origin_x_m, -10.0);
  EXPECT_DOUBLE_EQ(*telemetry.map_origin_y_m, -20.0);
  EXPECT_EQ(*telemetry.map_height_pixels, 1024);
  EXPECT_EQ(telemetry.diagnostics.at("motion_state"), 16);
  EXPECT_EQ(telemetry.diagnostics.at("charge_state"), 7);
  EXPECT_EQ(telemetry.diagnostics.at("dock_state"), 4);
}

TEST(VendorBaseTest, PollFailureRetainsExplicitlyStaleSampleWithoutLatchingBackend) {
  class FailingStatusDriver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      ++polls;
      if (fail.load()) throw std::runtime_error("status offline");
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 4.0, .map_resolution_m = 0.1, .battery_percent = 80, .location = 0};
    }

    std::atomic<bool> fail{false};
    std::atomic<int> polls{0};
  };

  auto driver = std::make_unique<FailingStatusDriver>();
  FailingStatusDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 5ms);
  ASSERT_TRUE(WaitFor([&] {
    const auto status = backend.ReadBaseTelemetry();
    return status.has_value() && status->fresh;
  }));
  const int successful_polls = observed->polls.load();
  observed->fail = true;
  ASSERT_TRUE(WaitFor([&] { return observed->polls.load() > successful_polls; }));
  ASSERT_TRUE(WaitFor([&] {
    const auto status = backend.ReadBaseTelemetry();
    return status.has_value() && !status->fresh;
  }));

  const auto stale = *backend.ReadBaseTelemetry();
  EXPECT_FALSE(stale.valid);
  EXPECT_FALSE(stale.fresh);
  EXPECT_DOUBLE_EQ(stale.x_m, 4.0);
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "still-works"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->stand, 1);
}

TEST(VendorBaseTest, SlowStatusPollDoesNotBlockDispatcherOperations) {
  class SlowStatusDriver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      polling = true;
      std::this_thread::sleep_for(150ms);
      return eos::backends::vendor::VendorBaseStatus{};
    }
    std::atomic<bool> polling{false};
  };

  auto driver = std::make_unique<SlowStatusDriver>();
  SlowStatusDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 500ms);
  ASSERT_TRUE(WaitFor([&] { return observed->polling.load(); }));
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "nonblocking"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms);
}

TEST(VendorBaseTest, FailuresAndHaltLatchHealth) {
  class ThrowingDriver final : public FakeDriver {
   public:
    void Stand() override { throw std::runtime_error("offline"); }
  };

  auto driver = std::make_unique<ThrowingDriver>();
  ThrowingDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "stand-fail"}),
            eos::hardware::ReleaseResult::kRejected);
  EXPECT_FALSE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.StopMotion(), eos::hardware::ReleaseResult::kAccepted);

  backend.Halt();
  backend.Halt();
  EXPECT_EQ(observed->halt, 1);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kWalk, "walk-halted"}),
            eos::hardware::ReleaseResult::kRejected);
}

TEST(VendorBaseTest, NavigationRefusalDoesNotLatchHealth) {
  class RejectingNavigationDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateRelative(double x_m, double y_m, double yaw_rad) override {
      FakeDriver::NavigateRelative(x_m, y_m, yaw_rad);
      return eos::hardware::ReleaseResult::kRejected;
    }
  };

  auto driver = std::make_unique<RejectingNavigationDriver>();
  RejectingNavigationDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  const eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
                                                              std::chrono::steady_clock::now()};

  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kRejected);
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.ReleaseFailureReason(), "vendor base refused the relative navigation goal");
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(observed->navigate_relative, 2);
}

TEST(VendorBaseTest, NavigationPreflightErrorIsActionableAndDoesNotLatchHealth) {
  class UnlocalizedDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateRelative(double, double, double) override {
      throw std::runtime_error("relative navigation requires normal localization");
    }
  };

  auto driver = std::make_unique<UnlocalizedDriver>();
  UnlocalizedDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  const eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
                                                              std::chrono::steady_clock::now()};

  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kRejected);
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.ReleaseFailureReason(), "relative navigation requires normal localization");
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kLie, "lie"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->lie, 1);
}

TEST(VendorBaseTest, RejectsInvalidCommandsBeforeCallingVendorDriver) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));

  const eos::hardware::BaseNavigateRelativeCommand invalid_navigation{
      std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, std::chrono::steady_clock::now()};
  EXPECT_EQ(backend.ExecuteNavigateRelative(invalid_navigation),
            eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(backend.ReleaseFailureReason(), "relative navigation target is outside vendor limits");
  EXPECT_EQ(observed->navigate_relative, 0);

  EXPECT_EQ(
      backend.ExecutePosture({static_cast<eos::hardware::BasePosture>(255), "invalid-posture"}),
      eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(backend.ReleaseFailureReason(), "unsupported vendor base posture");
  EXPECT_EQ(observed->stand + observed->walk + observed->lie, 0);
  EXPECT_TRUE(backend.ReadHealth().alive);
}

TEST(VendorBaseTest, RejectsStreamingNavigationWithoutResolvedTarget) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
                                                        std::chrono::steady_clock::now()};
  navigation.stream = true;

  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(backend.ReleaseFailureReason(), "stream navigation requires a map target");
  EXPECT_EQ(observed->navigate_relative, 0);
  EXPECT_TRUE(backend.ReadHealth().alive);
}

TEST(VendorBaseTest, StreamReplacesNavigationGoalThenHoldsCurrentPoseWithoutPosture) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = pose_x.load(), .y_m = -0.4, .yaw_rad = 0.2,
          .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>& target, double) override {
      last_x = target[0];
      last_y = target[1];
      last_yaw = target[2];
      ++starts;
    }
    void CancelNavigationTarget() override { ++cancels; }
    std::atomic<double> pose_x{0.0};
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
    std::atomic<double> last_x{0};
    std::atomic<double> last_y{0};
    std::atomic<double> last_yaw{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "first", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  navigation.command_id = "nearby";
  navigation.stream_map_target = std::array<double, 3>{0.32, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.32);
  navigation.command_id = "second";
  navigation.stream_map_target = std::array<double, 3>{0.6, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 3; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.6);
  EXPECT_GE(observed->cancels.load(), 1);
  navigation.command_id = "reverse";
  navigation.stream_map_target = std::array<double, 3>{-0.24, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 4; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), -0.24);
  observed->pose_x = 0.12;
  ASSERT_TRUE(WaitFor([&] {
    const auto pose = backend.ReadBaseTelemetry();
    return pose && pose->x_m == 0.12;
  }));
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 5; }, 800ms));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.12);
  EXPECT_DOUBLE_EQ(observed->last_y.load(), -0.4);
  EXPECT_DOUBLE_EQ(observed->last_yaw.load(), 0.2);
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(observed->starts.load(), 5);
  EXPECT_GE(observed->cancels.load(), 4);
  navigation.command_id = "after-hold";
  navigation.stream_map_target = std::array<double, 3>{0.8, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 6; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.8);
  EXPECT_EQ(observed->stand, 0);
  EXPECT_EQ(observed->walk, 0);
}

TEST(VendorBaseTest, HoldUsesPoseMeasuredAfterCancellationSettles) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = pose_x.load(), .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>& target, double) override {
      last_x = target[0];
      ++starts;
    }
    void WaitForNavigationIdle() override { pose_x = 0.45; }
    std::atomic<double> pose_x{0.0};
    std::atomic<double> last_x{0.0};
    std::atomic<int> starts{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.8, 0.0, 0.0,
      std::chrono::steady_clock::now(), "moving", true,
      std::array<double, 3>{0.8, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }, 800ms));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.45);
  EXPECT_EQ(observed->stand, 0);
}

TEST(VendorBaseTest, StreamCoalescesUpdatesWhilePreviousGoalSettles) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .yaw_rad = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>& target, double) override {
      last_x = target[0];
      ++starts;
    }
    void CancelNavigationTarget() override { ++cancels; }
    void WaitForNavigationIdle() override {
      settling = true;
      while (!settled) std::this_thread::sleep_for(1ms);
    }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
    std::atomic<double> last_x{0.0};
    std::atomic_bool settling{false};
    std::atomic_bool settled{false};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "first", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  navigation.command_id = "second";
  navigation.stream_map_target = std::array<double, 3>{0.6, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  const bool entered_settlement = WaitFor([&] { return observed->settling.load(); });
  navigation.command_id = "latest";
  navigation.stream_map_target = std::array<double, 3>{-0.5, 0.0, 0.0};
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms);
  observed->settled = true;
  ASSERT_TRUE(entered_settlement);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), -0.5);
  EXPECT_EQ(observed->stand, 0);
  EXPECT_EQ(observed->walk, 0);
}

TEST(VendorBaseTest, IdleStreamSessionExpiresAndLaterRequestStartsFreshGoal) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.1, .y_m = 0.2, .yaw_rad = 0.3,
          .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>& target, double) override {
      last_x = target[0];
      ++starts;
    }
    void CancelNavigationTarget() override { ++cancels; }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
    std::atomic<double> last_x{0.0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms, 100ms, 300ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "first", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }, 800ms));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), 0.1);
  ASSERT_TRUE(WaitFor([&] { return observed->cancels.load() >= 2; }, 800ms));
  EXPECT_EQ(observed->stand, 0);
  navigation.command_id = "restart";
  navigation.stream_map_target = std::array<double, 3>{-0.3, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 3; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), -0.3);
}

TEST(VendorBaseTest, StreamAcceptsNextPointAfterArrivalAndStopsOnStalePose) {
  class Driver final : public FakeDriver {
   public:
    void Stand() override { CancelNavigationTarget(); FakeDriver::Stand(); }
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      if (fail_status) throw std::runtime_error("status unavailable");
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = x.load(), .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override { ++starts; }
    void CancelNavigationTarget() override { ++cancels; }
    std::atomic<double> x{0.0};
    std::atomic_bool fail_status{false};
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "arrival", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  observed->x = 0.3;
  ASSERT_TRUE(WaitFor([&] {
    const auto pose = backend.ReadBaseTelemetry();
    return pose && pose->x_m == 0.3;
  }));
  navigation.command_id = "next-after-arrival";
  navigation.stream_map_target = std::array<double, 3>{0.6, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }));
  observed->fail_status = true;
  ASSERT_TRUE(WaitFor([&] {
    const auto pose = backend.ReadBaseTelemetry();
    return pose && !pose->fresh;
  }));
  ASSERT_TRUE(WaitFor([&] { return observed->cancels.load() >= 2; }));
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kRejected);
}

TEST(VendorBaseTest, SmallDirectionReversalReplacesGoal) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .yaw_rad = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>& target, double) override {
      last_x = target[0];
      ++starts;
    }
    void CancelNavigationTarget() override { ++cancels; }
    std::atomic<double> last_x{0};
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.06, 0.0, 0.0,
      std::chrono::steady_clock::now(), "forward", true,
      std::array<double, 3>{0.06, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  navigation.command_id = "reverse";
  navigation.stream_map_target = std::array<double, 3>{-0.06, 0.0, 0.0};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 2; }));
  EXPECT_DOUBLE_EQ(observed->last_x.load(), -0.06);
  EXPECT_GE(observed->cancels.load(), 1);
  navigation.command_id = "rotate";
  navigation.stream_map_target = std::array<double, 3>{0.0, 0.0, 0.06};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 3; }));
  navigation.command_id = "reverse-rotate";
  navigation.stream_map_target = std::array<double, 3>{0.0, 0.0, -0.06};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 4; }));
}

TEST(VendorBaseTest, StandCancelsNavigationAndStopUsesSoftEmergencyPath) {
  class Driver final : public FakeDriver {
   public:
    void Stand() override { CancelNavigationTarget(); FakeDriver::Stand(); }
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override { ++starts; }
    void CancelNavigationTarget() override { ++cancels; }
    void SoftEmergencyStop() override { ++emergency_stops; }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
    std::atomic<int> emergency_stops{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "held", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "released"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_GE(observed->cancels.load(), 1);
  EXPECT_EQ(observed->stand, 1);
  EXPECT_EQ(observed->emergency_stops.load(), 0);
  EXPECT_EQ(backend.StopMotion(), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->emergency_stops.load(), 1);
}

TEST(VendorBaseTest, FailedNavigationSubmissionRetainsStopAndPosture) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override {
      ++attempts;
      throw std::runtime_error("navigation already active or unavailable");
    }
    void SoftEmergencyStop() override { ++emergency_stops; }
    std::atomic<int> attempts{0};
    std::atomic<int> emergency_stops{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "failure", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->attempts.load() > 0; }));
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.StopMotion(), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->emergency_stops.load(), 1);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "stand"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_TRUE(backend.ReadHealth().alive);
}

TEST(VendorBaseTest, VendorGoalFailureCanRecoverWithoutPostureChange) {
  class Driver final : public FakeDriver {
   public:
    void Stand() override { CancelNavigationTarget(); FakeDriver::Stand(); }
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override { ++starts; }
    void CancelNavigationTarget() override { ++cancels; }
    std::optional<int> NavigationTargetOutcome() const override {
      if (starts.load() > 0) return 2;
      return std::nullopt;
    }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "failed-result", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->cancels.load() > 0; }));
  navigation.command_id = "recover";
  navigation.stream_map_target = std::array<double, 3>{0.6, 0.0, 0.0};
  ASSERT_TRUE(WaitFor([&] {
    return backend.ExecuteNavigateRelative(navigation) == eos::hardware::ReleaseResult::kAccepted;
  }));
  EXPECT_EQ(observed->stand, 0);
  EXPECT_EQ(observed->walk, 0);
}

TEST(VendorBaseTest, SoftEmergencySignalPreemptsBlockedStreamSettlement) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override { ++starts; }
    void CancelNavigationTarget() override { ++cancels; }
    void WaitForNavigationIdle() override {
      settling = true;
      while (!release_settlement) std::this_thread::sleep_for(1ms);
    }
    void EmergencySignal() override { ++emergency_signals; }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
    std::atomic_bool settling{false};
    std::atomic_bool release_settlement{false};
    std::atomic<int> emergency_signals{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "held", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  ASSERT_TRUE(WaitFor([&] { return observed->settling.load(); }, 1s));
  auto stopped = std::async(std::launch::async, [&] { return backend.StopMotion(); });
  const bool signaled = WaitFor([&] { return observed->emergency_signals.load() == 1; }, 100ms);
  observed->release_settlement = true;
  EXPECT_TRUE(signaled);
  EXPECT_EQ(stopped.get(), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->stand, 0);
}

TEST(VendorBaseTest, OrdinaryNavigationCancelsStreamFirst) {
  class Driver final : public FakeDriver {
   public:
    std::optional<eos::backends::vendor::VendorBaseStatus> ReadStatus() override {
      return eos::backends::vendor::VendorBaseStatus{
          .x_m = 0.0, .map_resolution_m = 0.05, .location = 0};
    }
    void StartNavigationTarget(const std::array<double, 3>&, double) override { ++starts; }
    void CancelNavigationTarget() override { ++cancels; }
    std::atomic<int> starts{0};
    std::atomic<int> cancels{0};
  };
  auto driver = std::make_unique<Driver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver), 20ms);
  ASSERT_TRUE(WaitFor([&] { return backend.ReadBaseTelemetry().has_value(); }));
  eos::hardware::BaseNavigateRelativeCommand navigation{0.3, 0.0, 0.0,
      std::chrono::steady_clock::now(), "stream", true,
      std::array<double, 3>{0.3, 0.0, 0.0}};
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  ASSERT_TRUE(WaitFor([&] { return observed->starts.load() == 1; }));
  navigation.stream = false;
  navigation.stream_map_target.reset();
  EXPECT_EQ(backend.ExecuteNavigateRelative(navigation), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_GE(observed->cancels.load(), 1);
  EXPECT_EQ(observed->navigate_relative, 1);
}

TEST(VendorBaseTest, ClampDoesNotLatchBackend) {
  auto driver = std::make_unique<FakeDriver>();
  FakeDriver* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));

  backend.Clamp();

  EXPECT_EQ(observed->clamp, 1);
  EXPECT_TRUE(backend.ReadHealth().alive);
  EXPECT_EQ(backend.ExecutePosture({eos::hardware::BasePosture::kStand, "stand-after-clamp"}),
            eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(observed->stand, 1);
}

TEST(VendorBaseTest, CancelsOnlyMatchingActiveNavigation) {
  class CancellableDriver final : public FakeDriver {
   public:
    eos::hardware::ReleaseResult NavigateRelative(double, double, double) override {
      entered = true;
      while (!cancelled) std::this_thread::sleep_for(1ms);
      return eos::hardware::ReleaseResult::kRejected;
    }
    eos::hardware::ReleaseResult CancelNavigation() override {
      ++cancel_calls;
      cancelled = true;
      return eos::hardware::ReleaseResult::kAccepted;
    }
    std::atomic_bool entered{false};
    std::atomic_bool cancelled{false};
    std::atomic_int cancel_calls{0};
  };
  auto driver = std::make_unique<CancellableDriver>();
  auto* observed = driver.get();
  eos::backends::vendor::VendorBase backend("base-0", std::move(driver));
  eos::hardware::BaseNavigateRelativeCommand command{0.3, 0.0, 0.0,
                                                     std::chrono::steady_clock::now(), "goal-1"};
  auto result =
      std::async(std::launch::async, [&] { return backend.ExecuteNavigateRelative(command); });
  ASSERT_TRUE(WaitFor([&] { return observed->entered.load(); }));
  EXPECT_EQ(backend.CancelCommand("other-goal"), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(observed->cancel_calls, 0);
  EXPECT_EQ(backend.CancelCommand("goal-1"), eos::hardware::ReleaseResult::kAccepted);
  EXPECT_EQ(result.get(), eos::hardware::ReleaseResult::kRejected);
  EXPECT_EQ(observed->cancel_calls, 1);
  EXPECT_EQ(backend.CancelCommand("goal-1"), eos::hardware::ReleaseResult::kRejected);
}

}  // namespace

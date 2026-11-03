/// Private adapter from EOS base posture commands to the Island Base API.
/// The public capability surface deliberately excludes base.velocity: the
/// vendor manual-motion API accepts unitless joystick axes, not SI velocity,
/// so it cannot faithfully implement eos::hardware::BaseCommand.
#pragma once

#include <chrono>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "eos/hardware/backend.h"

namespace eos::backends::vendor {

class NavigationSettlingTimeout final : public std::runtime_error {
 public:
  NavigationSettlingTimeout()
      : std::runtime_error("timed out waiting for navigation cancellation to settle") {}
};

struct VendorBaseConfig {
  std::string motion_host;
  std::uint16_t motion_port;
  std::string status_bind_host;
  std::uint16_t status_port;
  std::string expected_sender;
  std::string robotserver_host;
  std::uint16_t robotserver_port;
  std::uint64_t io_timeout_ms;
  std::uint64_t navigation_timeout_ms;
  std::uint64_t transition_timeout_ms;
  std::uint64_t idle_stand_ms;
  int map_id;
  std::optional<std::string> obstacle_host;
  std::uint16_t obstacle_port{0};
  std::uint64_t obstacle_timeout_ms{0};
  std::uint64_t obstacle_max_age_ms{0};
  int stream_axis_limit{12000};
  double stream_position_tolerance_m{0.05};
  double stream_yaw_tolerance_rad{0.05};
  double stream_retarget_distance_m{0.20};
  double stream_retarget_yaw_rad{0.30};
};

/// Raw fields returned by Island Base Type 1002. Unit and semantic
/// normalization belongs to VendorBase rather than to test doubles.
struct VendorBaseStatus {
  int motion_state{0};
  double x_m{0.0};
  double y_m{0.0};
  double z_m{0.0};
  double roll_rad{0.0};
  double pitch_rad{0.0};
  double yaw_rad{0.0};
  double speed_m_s{0.0};
  double current_odometry_km{0.0};
  double total_odometry_km{0.0};
  int current_runtime{0};
  int total_runtime{0};
  double map_resolution_m{0.0};
  double map_origin_x_m{0.0};
  double map_origin_y_m{0.0};
  double map_height_pixels{0.0};
  int battery_percent{0};
  int location{0};
  int rtk_state{0};
  int dock_state{0};
  int gait_state{0};
  int motor_state{0};
  int charge_state{0};
  int control_mode{0};
  int map_update_state{0};
};

/// Narrow driver seam used to keep the EOS adapter independently testable.
/// The production implementation delegates only to documented Island Base
/// controllers; vendor protocol details do not enter EOS.
class VendorBaseDriver {
 public:
  virtual ~VendorBaseDriver() = default;
  virtual void Stand() = 0;
  virtual void Walk() = 0;
  virtual void Lie() = 0;
  /// Returns rejected for a completed vendor refusal; transport failures throw.
  virtual hardware::ReleaseResult NavigateRelative(double x_m, double y_m, double yaw_rad) = 0;
  virtual hardware::ReleaseResult NavigateRelativeWithGait(double x_m, double y_m,
                                                           double yaw_rad, int gait) {
    if (gait != 0) throw std::runtime_error("relative navigation gait is unavailable");
    return NavigateRelative(x_m, y_m, yaw_rad);
  }
  /// Sends an absolute map-frame target through the persistent navigation session.
  virtual hardware::ReleaseResult NavigateAbsolute(double, double, double, double, int) {
    throw std::runtime_error("absolute navigation is unavailable");
  }
  virtual hardware::ReleaseResult NavigateAbsoluteWithGait(double x_m, double y_m, double z_m,
                                                           double yaw_rad, int speed, int gait) {
    if (gait != 0) throw std::runtime_error("absolute navigation gait is unavailable");
    return NavigateAbsolute(x_m, y_m, z_m, yaw_rad, speed);
  }
  virtual void SetGait(const std::string&) {
    throw std::runtime_error("standalone gait switching is unavailable");
  }
  virtual void PrepareNavigation() {}
  /// May run concurrently with a blocking NavigateRelative call.
  virtual hardware::ReleaseResult CancelNavigation() { return hardware::ReleaseResult::kRejected; }
  /// RobotServer task navigation. A new goal replaces the previous task with Type 1004.
  virtual void StartNavigationTarget(const std::array<double, 3>&, double) {
    throw std::runtime_error("stream navigation is unavailable");
  }
  virtual void StartNavigationTargetWithSpeed(const std::array<double, 3>& target,
                                              double z_m, int speed) {
    if (speed != 1) throw std::runtime_error("stream navigation speed is unavailable");
    StartNavigationTarget(target, z_m);
  }
  virtual void CancelNavigationTarget() {}
  /// Waits for the cancelled goal's final reply and an idle controller state.
  virtual void WaitForNavigationIdle() {}
  /// No value while pending; zero is success, positive is vendor refusal, -1 is uncertain.
  virtual std::optional<int> NavigationTargetOutcome() const { return std::nullopt; }
  /// Sends the vendor soft emergency command without waiting for navigation I/O.
  virtual void EmergencySignal() {}
  virtual void SoftEmergencyStop() { Clamp(); }
  /// Performs one bounded Type 1002 query in production. The adapter invokes
  /// this only from its status worker, never on a dispatcher call path.
  virtual std::optional<VendorBaseStatus> ReadStatus() { return std::nullopt; }
  virtual bool SupportsObstacles() const { return false; }
  virtual hardware::ObstacleSnapshot ReadObstacles() const {
    return {};
  }
  virtual void SetAxes(int forward, int right, int yaw) {
    throw std::runtime_error("manual motion axes are unavailable");
  }
  virtual void Clamp() = 0;
  virtual void Halt() = 0;
};

class VendorBase final : public hardware::Backend {
 public:
  VendorBase(std::string device_id, VendorBaseConfig config);
  VendorBase(std::string device_id, std::unique_ptr<VendorBaseDriver> driver,
             std::chrono::milliseconds status_poll_interval = std::chrono::milliseconds(500),
             std::chrono::milliseconds stream_inactivity_timeout = std::chrono::milliseconds(100),
             std::chrono::milliseconds stream_idle_session_timeout = std::chrono::seconds(30));
  ~VendorBase() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::BackendHealth ReadHealth() const override;
  bool CanStopWithoutDeviceLock() const override { return true; }
  std::optional<hardware::BaseTelemetry> ReadBaseTelemetry() const override;
  bool SupportsObstacleSnapshot() const override;
  std::optional<hardware::ObstacleSnapshot> ReadObstacleSnapshot() const override;
  hardware::ReleaseResult ExecuteNavigateRelative(
      const hardware::BaseNavigateRelativeCommand& command) override;
  hardware::ReleaseResult ExecuteGait(const hardware::BaseGaitCommand& command) override;
  hardware::ReleaseResult ExecutePosture(const hardware::BasePostureCommand& command) override;
  hardware::ReleaseResult StopMotion() override;
  hardware::ReleaseResult CancelCommand(const std::string& command_id) override;
  std::string ReleaseFailureReason() const override;
  void Clamp() override;
  void Halt() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::vendor

#include "vendor_base.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <island/manual_motion.hpp>
#include <island/gait.hpp>
#include <island/navigation.hpp>
#include <island/obstacles.hpp>
#include <island/pose.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "adapters/eos-cxx/lib/log.h"

namespace eos::backends::vendor {
namespace {

constexpr int kAutomaticNavigationManner = -1;

auto VendorLogger() { return eos::lib::GetLogger("vendor"); }

const char* PostureName(hardware::BasePosture posture) {
  switch (posture) {
    case hardware::BasePosture::kStand:
      return "stand";
    case hardware::BasePosture::kWalk:
      return "walk";
    case hardware::BasePosture::kLie:
      return "lie";
  }
  return "unknown";
}

class RobotServerNavigationRejection final : public std::runtime_error {
 public:
  RobotServerNavigationRejection(int error_code, int error_status)
      : std::runtime_error(
            "RobotServer rejected relative navigation (error_code=" + std::to_string(error_code) +
            ", error_status=" + std::to_string(error_status) + ")"),
        error_code(error_code),
        error_status(error_status) {}

  int error_code;
  int error_status;
};

class IslandBaseDriver final : public VendorBaseDriver {
 public:
  explicit IslandBaseDriver(const VendorBaseConfig& config)
      : poses_(island::PoseEndpoints{
            config.motion_host, config.motion_port, config.status_bind_host, config.status_port,
            config.expected_sender, config.robotserver_host, config.robotserver_port,
            std::chrono::milliseconds(config.io_timeout_ms)}),
        navigation_(island::NavigationEndpoints{
            config.robotserver_host, config.robotserver_port,
            std::chrono::milliseconds(config.navigation_timeout_ms), config.map_id}),
        navigation_endpoints_{config.robotserver_host, config.robotserver_port,
                              std::chrono::milliseconds(config.io_timeout_ms), config.map_id},
        relative_navigation_result_timeout_(config.navigation_timeout_ms),
        navigation_result_timeout_(std::min(std::chrono::milliseconds(config.navigation_timeout_ms),
                                            std::chrono::milliseconds(30000))),
        status_(island::NavigationEndpoints{config.robotserver_host, config.robotserver_port,
                                            std::chrono::milliseconds(config.io_timeout_ms),
                                            config.map_id}),
        manual_endpoints_{config.motion_host,
                          config.motion_port,
                          config.robotserver_host,
                          config.robotserver_port,
                          std::chrono::milliseconds(config.io_timeout_ms),
                          std::chrono::milliseconds(config.transition_timeout_ms),
                          std::chrono::milliseconds(config.idle_stand_ms)} {
    if (config.obstacle_host) {
      obstacles_.emplace(island::ObstacleEndpoints{
          *config.obstacle_host, config.obstacle_port,
          std::chrono::milliseconds(config.obstacle_timeout_ms),
          std::chrono::milliseconds(config.obstacle_max_age_ms)});
    }
  }

  bool SupportsObstacles() const override { return obstacles_.has_value(); }

  hardware::ObstacleSnapshot ReadObstacles() const override {
    const island::ObstacleSnapshot source = obstacles_->GetSnapshot();
    hardware::ObstacleSnapshot result;
    switch (source.state) {
      case island::ObstacleState::kUnavailable:
        result.state = hardware::ObstacleState::kUnavailable;
        break;
      case island::ObstacleState::kFresh:
        result.state = hardware::ObstacleState::kFresh;
        break;
      case island::ObstacleState::kStale:
        result.state = hardware::ObstacleState::kStale;
        break;
      case island::ObstacleState::kSourceError:
        result.state = hardware::ObstacleState::kSourceError;
        break;
    }
    result.age_s = source.age_s;
    if (std::isfinite(source.source_stamp_s) && source.source_stamp_s > 0.0) {
      result.source_stamp_s = source.source_stamp_s;
    }
    if (result.state == hardware::ObstacleState::kFresh) {
      for (std::size_t i = 0; i < result.sectors.size(); ++i) {
        result.sectors[i].point_count = source.sectors[i].point_count;
        if (source.sectors[i].point_count > 0 && source.sectors[i].distance_m &&
            std::isfinite(*source.sectors[i].distance_m) &&
            *source.sectors[i].distance_m >= 0.0) {
          result.sectors[i].distance_m = source.sectors[i].distance_m;
        }
      }
    }
    return result;
  }

  ~IslandBaseDriver() override {
    try { CancelNavigationTarget(); } catch (...) {}
    if (navigation_result_thread_.joinable()) navigation_result_thread_.join();
  }

  void Stand() override {
    WaitForNavigationIdle();
    EndMotionSession();
    poses_.Execute(island::Pose::kStand);
    standing_heartbeat_ = std::make_unique<island::HandheldHeartbeat>(
        manual_endpoints_.motion_host, manual_endpoints_.motion_port, manual_endpoints_.timeout);
  }

  void Walk() override {
    if (manual_motion_ != nullptr) {
      manual_motion_->Resume();
      return;
    }
    // The manual controller only accepts states 2-4. A stream may begin while
    // the robot is prone, so bring it to a verified stand first.
    const island::NavigationInfo info = status_.GetInfo();
    if (info.motion_state == 0) Stand();
    auto session = std::make_unique<island::ManualMotionController>(manual_endpoints_);
    session->Start();
    standing_heartbeat_.reset();
    manual_motion_ = std::move(session);
  }

  void Lie() override {
    WaitForNavigationIdle();
    EndMotionSession();
    poses_.Execute(island::Pose::kProne);
  }

  hardware::ReleaseResult NavigateRelative(double x_m, double y_m, double yaw_rad) override {
    return NavigateRelativeWithGait(x_m, y_m, yaw_rad, 0);
  }

  hardware::ReleaseResult NavigateRelativeWithGait(double x_m, double y_m, double yaw_rad,
                                                   int gait) override {
    WaitForNavigationIdle();
    EndMotionSession();
    const island::NavigationInfo pose = navigation_.GetInfo();
    if (pose.location != 0) {
      throw std::runtime_error("relative navigation requires normal localization");
    }
    const double target_x = pose.pos_x + std::cos(pose.yaw) * x_m - std::sin(pose.yaw) * y_m;
    const double target_y = pose.pos_y + std::sin(pose.yaw) * x_m + std::cos(pose.yaw) * y_m;
    return NavigateTarget({target_x, target_y, pose.pos_z,
                           std::remainder(pose.yaw + yaw_rad, 2.0 * std::acos(-1.0)),
                           1, kAutomaticNavigationManner, gait});
  }

  hardware::ReleaseResult NavigateAbsolute(double x_m, double y_m, double z_m,
                                           double yaw_rad, int speed) override {
    return NavigateAbsoluteWithGait(x_m, y_m, z_m, yaw_rad, speed, 0);
  }

  hardware::ReleaseResult NavigateAbsoluteWithGait(double x_m, double y_m, double z_m,
                                                   double yaw_rad, int speed, int gait) override {
    WaitForNavigationIdle();
    EndMotionSession();
    return NavigateTarget({x_m, y_m, z_m, yaw_rad, speed, kAutomaticNavigationManner, gait});
  }

  void SetGait(const std::string& name) override {
    WaitForNavigationIdle();
    const island::Gait gait = [&] {
      if (name == "walk") return island::Gait::kWalk;
      if (name == "slope") return island::Gait::kSlope;
      if (name == "obstacle") return island::Gait::kObstacle;
      if (name == "stairs") return island::Gait::kStairs;
      if (name == "accumulated_stairs") return island::Gait::kAccumulatedStairs;
      if (name == "accumulated_steep_stairs") return island::Gait::kAccumulatedSteepStairs;
      if (name == "l_walk") return island::Gait::kLWalk;
      if (name == "mountain") return island::Gait::kMountain;
      if (name == "quiet") return island::Gait::kQuiet;
      throw std::invalid_argument("unsupported standalone gait");
    }();
    island::GaitController controller({
        manual_endpoints_.motion_host, manual_endpoints_.motion_port,
        manual_endpoints_.robotserver_host, manual_endpoints_.robotserver_port,
        manual_endpoints_.timeout, manual_endpoints_.transition_timeout,
        island::GaitSource::kAutonomousModule});
    controller.Set(gait);
  }

 private:
  hardware::ReleaseResult NavigateTarget(const island::NavigationTarget& target) {
    auto session = std::make_shared<island::NavigationSession>(
        navigation_endpoints_, relative_navigation_result_timeout_);
    {
      std::lock_guard<std::mutex> lock(relative_navigation_mutex_);
      relative_navigation_session_ = session;
      if (relative_navigation_cancel_requested_) session->RequestStop();
    }
    island::NavigationResult result{};
    try {
      session->Start(target);
      result = session->WaitResult();
    } catch (...) {
      try {
        session->Cancel();
      } catch (...) {
        // Preserve the original navigation failure; cancellation is best effort.
      }
      std::lock_guard<std::mutex> lock(relative_navigation_mutex_);
      relative_navigation_session_.reset();
      relative_navigation_cancel_requested_ = false;
      throw;
    }
    {
      std::lock_guard<std::mutex> lock(relative_navigation_mutex_);
      relative_navigation_session_.reset();
      relative_navigation_cancel_requested_ = false;
    }
    if (result.error_code != 0) {
      throw RobotServerNavigationRejection(result.error_code, result.error_status);
    }
    return hardware::ReleaseResult::kAccepted;
  }

 public:

  void PrepareNavigation() override {
    std::lock_guard<std::mutex> lock(relative_navigation_mutex_);
    relative_navigation_cancel_requested_ = false;
  }

  hardware::ReleaseResult CancelNavigation() override {
    std::shared_ptr<island::NavigationSession> session;
    {
      std::lock_guard<std::mutex> lock(relative_navigation_mutex_);
      relative_navigation_cancel_requested_ = true;
      session = relative_navigation_session_;
    }
    if (session) session->Cancel();
    return hardware::ReleaseResult::kAccepted;
  }

  void StartNavigationTarget(const std::array<double, 3>& target, double z_m) override {
    StartNavigationTargetWithSpeed(target, z_m, 1);
  }

  void StartNavigationTargetWithSpeed(const std::array<double, 3>& target, double z_m,
                                      int speed) override {
    if (navigation_session_) {
      WaitForNavigationIdle();
    }
    auto session = std::make_unique<island::NavigationSession>(navigation_endpoints_,
                                                               navigation_result_timeout_);
    navigation_session_ = std::move(session);
    navigation_cancel_sent_ = false;
    try {
      navigation_session_->Start(
          island::NavigationTarget{target[0], target[1], z_m, target[2], speed,
                                   kAutomaticNavigationManner});
    } catch (...) {
      try { CancelNavigationTarget(); } catch (...) {}
      throw;
    }
    navigation_result_done_ = false;
    navigation_result_thread_ = std::thread([this] {
      try {
        const auto result = navigation_session_->WaitResult();
        navigation_result_code_ = result.error_code;
      } catch (...) {
        navigation_result_code_ = -1;
      }
      navigation_result_done_ = true;
    });
  }

  void CancelNavigationTarget() override {
    if (!navigation_session_) return;
    if (navigation_cancel_sent_) return;
    navigation_session_->RequestStop();
    navigation_session_->Cancel();
    navigation_cancel_sent_ = true;
  }

  std::optional<int> NavigationTargetOutcome() const override {
    if (!navigation_session_ || !navigation_result_done_) return std::nullopt;
    return navigation_result_code_.load();
  }

  void EmergencySignal() override { poses_.EmergencyStop(); }

  void SoftEmergencyStop() override {
    std::string cancel_error;
    try {
      CancelNavigationTarget();
    } catch (const std::exception& error) {
      cancel_error = error.what();
    }
    try {
      EndMotionSession();
    } catch (const std::exception& error) {
      if (cancel_error.empty()) cancel_error = error.what();
    }
    if (!cancel_error.empty()) {
      eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_navigation_cancel_failed",
                         {{"reason", cancel_error}});
    }
  }

  void WaitForNavigationIdle() override {
    CancelNavigationTarget();
    if (!navigation_session_) return;
    // RobotServer can acknowledge Type 1004 well before the final Type 1003
    // reply. Keep stop available while waiting, and check once more at the
    // deadline so a reply arriving on the boundary is not lost.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
    do {
      if (navigation_result_done_ && navigation_session_->Status().status == 0) {
        if (navigation_result_thread_.joinable()) navigation_result_thread_.join();
        navigation_session_.reset();
        return;
      }
      if (std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (true);
    throw NavigationSettlingTimeout();
  }

  std::optional<VendorBaseStatus> ReadStatus() override {
    const island::NavigationInfo info = status_.GetInfo();
    return VendorBaseStatus{
        .motion_state = info.motion_state,
        .x_m = info.pos_x,
        .y_m = info.pos_y,
        .z_m = info.pos_z,
        .roll_rad = info.roll,
        .pitch_rad = info.pitch,
        .yaw_rad = info.yaw,
        .speed_m_s = info.speed,
        .current_odometry_km = info.current_odometry,
        .total_odometry_km = info.total_odometry,
        .current_runtime = info.current_runtime,
        .total_runtime = info.total_runtime,
        .map_resolution_m = info.map_resolution,
        .map_origin_x_m = info.map_origin_x,
        .map_origin_y_m = info.map_origin_y,
        .map_height_pixels = info.map_height,
        .battery_percent = info.battery,
        .location = info.location,
        .rtk_state = info.rtk_state,
        .dock_state = info.dock_state,
        .gait_state = info.gait_state,
        .motor_state = info.motor_state,
        .charge_state = info.charge_state,
        .control_mode = info.control_mode,
        .map_update_state = info.map_update_state,
    };
  }

  void Clamp() override {
    if (manual_motion_ != nullptr) manual_motion_->Stop();
  }

  void SetAxes(int forward, int right, int yaw) override {
    if (manual_motion_ == nullptr) throw std::runtime_error("manual motion session is unavailable");
    manual_motion_->SetAxes(forward, right, yaw);
  }

  void Halt() override {
    std::string stop_error;
    try {
      CancelNavigationTarget();
      EndMotionSession();
    } catch (const std::exception& error) {
      // A failed zero-axis send must not suppress the independent soft-estop
      // attempt. Destruction performs its own best-effort cleanup.
      stop_error = error.what();
      manual_motion_.reset();
    } catch (...) {
      stop_error = "unknown_exception";
      manual_motion_.reset();
    }
    if (!stop_error.empty()) {
      eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_manual_motion_stop_failed",
                         {{"reason", stop_error}});
    }
  }

 private:
  void EndManualMotion() {
    if (manual_motion_ != nullptr) manual_motion_->Stop();
    manual_motion_.reset();
  }

  void EndMotionSession() {
    standing_heartbeat_.reset();
    EndManualMotion();
  }

  island::PoseController poses_;
  std::optional<island::ObstacleClient> obstacles_;
  island::NavigationController navigation_;
  std::mutex relative_navigation_mutex_;
  std::shared_ptr<island::NavigationSession> relative_navigation_session_;
  bool relative_navigation_cancel_requested_{false};
  island::NavigationEndpoints navigation_endpoints_;
  std::chrono::milliseconds relative_navigation_result_timeout_;
  std::chrono::milliseconds navigation_result_timeout_;
  std::unique_ptr<island::NavigationSession> navigation_session_;
  std::thread navigation_result_thread_;
  std::atomic_bool navigation_result_done_{true};
  std::atomic_int navigation_result_code_{0};
  bool navigation_cancel_sent_{false};
  island::NavigationController status_;
  island::ManualMotionEndpoints manual_endpoints_;
  std::unique_ptr<island::ManualMotionController> manual_motion_;
  std::unique_ptr<island::HandheldHeartbeat> standing_heartbeat_;
};

}  // namespace

struct VendorBase::Impl {
  explicit Impl(std::string id, std::unique_ptr<VendorBaseDriver> value,
                std::chrono::milliseconds interval, std::chrono::milliseconds inactivity,
                std::chrono::milliseconds idle_session)
      : identity{.name = "vendor-base",
                 .version = "1.0.0",
                 .device_id = std::move(id),
                 .kind = hardware::DeviceKind::kBase},
        driver(std::move(value)),
        status_poll_interval(interval), stream_inactivity_timeout(inactivity),
        stream_idle_session_timeout(idle_session) {}

  hardware::BackendIdentity identity;
  std::unique_ptr<VendorBaseDriver> driver;
  mutable std::mutex mutex;
  std::mutex driver_mutex;
  std::mutex navigation_command_mutex;
  std::string active_navigation_command_id;
  std::mutex status_mutex;
  std::condition_variable status_wakeup;
  std::thread status_worker;
  std::thread stream_worker;
  std::condition_variable stream_wakeup;
  bool stop_stream_worker{false};
  bool stream_active{false};
  bool stream_holding{false};
  bool stream_session_owned{false};
  bool stream_cleanup_pending{false};
  std::array<double, 3> stream_target{};
  std::array<double, 3> stream_submitted_target{};
  std::optional<double> stream_z_m;
  int stream_speed{1};
  double stream_submitted_z_m{0.0};
  int stream_submitted_speed{1};
  std::chrono::steady_clock::time_point stream_updated{};
  std::chrono::steady_clock::time_point stream_retry_at{};
  std::chrono::steady_clock::time_point stream_first_failure_at{};
  std::string stream_failure_reason;
  int stream_axis_limit{12000};
  double stream_position_tolerance_m{0.05};
  double stream_yaw_tolerance_rad{0.05};
  double stream_retarget_distance_m{0.20};
  double stream_retarget_yaw_rad{0.30};
  std::chrono::milliseconds status_poll_interval;
  std::chrono::milliseconds stream_inactivity_timeout;
  std::chrono::milliseconds stream_idle_session_timeout;
  std::optional<hardware::BaseTelemetry> status;
  std::chrono::steady_clock::time_point status_observed_steady{};
  bool status_failed{false};
  bool stop_status{false};
  std::atomic_bool alive{true};
  bool halted{false};
  std::string release_failure_reason{"vendor base refused the release"};
};

VendorBase::VendorBase(std::string device_id, VendorBaseConfig config)
    : VendorBase(std::move(device_id), std::make_unique<IslandBaseDriver>(config)) {
  if (config.stream_axis_limit < 1 || config.stream_axis_limit > 32767 ||
      !std::isfinite(config.stream_position_tolerance_m) ||
      config.stream_position_tolerance_m <= 0.0 ||
      !std::isfinite(config.stream_yaw_tolerance_rad) || config.stream_yaw_tolerance_rad <= 0.0 ||
      !std::isfinite(config.stream_retarget_distance_m) ||
      config.stream_retarget_distance_m <= config.stream_position_tolerance_m ||
      !std::isfinite(config.stream_retarget_yaw_rad) ||
      config.stream_retarget_yaw_rad <= config.stream_yaw_tolerance_rad) {
    throw std::invalid_argument("invalid vendor stream control limits");
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->stream_axis_limit = config.stream_axis_limit;
  impl_->stream_position_tolerance_m = config.stream_position_tolerance_m;
  impl_->stream_yaw_tolerance_rad = config.stream_yaw_tolerance_rad;
  impl_->stream_retarget_distance_m = config.stream_retarget_distance_m;
  impl_->stream_retarget_yaw_rad = config.stream_retarget_yaw_rad;
}

VendorBase::VendorBase(std::string device_id, std::unique_ptr<VendorBaseDriver> driver,
                       std::chrono::milliseconds status_poll_interval,
                       std::chrono::milliseconds stream_inactivity_timeout,
                       std::chrono::milliseconds stream_idle_session_timeout)
    : impl_(std::make_unique<Impl>(std::move(device_id), std::move(driver), status_poll_interval,
                                   stream_inactivity_timeout, stream_idle_session_timeout)) {
  if (impl_->driver == nullptr) throw std::invalid_argument("vendor base driver is required");
  if (status_poll_interval.count() <= 0) {
    throw std::invalid_argument("vendor base status poll interval must be positive");
  }
  if (stream_inactivity_timeout.count() <= 0 ||
      stream_idle_session_timeout <= stream_inactivity_timeout) {
    throw std::invalid_argument("invalid vendor stream timeouts");
  }
  impl_->status_worker = std::thread([this] {
    for (;;) {
      try {
        const std::optional<VendorBaseStatus> raw = impl_->driver->ReadStatus();
        std::optional<hardware::BaseTelemetry> sample;
        if (raw.has_value()) {
          hardware::BaseTelemetry normalized;
          normalized.x_m = raw->x_m;
          normalized.y_m = raw->y_m;
          normalized.yaw_rad = raw->yaw_rad;
          normalized.linear_x_m_s = raw->speed_m_s;
          normalized.observed_at = std::chrono::system_clock::now();
          normalized.valid = true;
          normalized.fresh = true;
          normalized.pose_valid = std::isfinite(raw->x_m) && std::isfinite(raw->y_m) &&
                                  std::isfinite(raw->z_m) && std::isfinite(raw->roll_rad) &&
                                  std::isfinite(raw->pitch_rad) && std::isfinite(raw->yaw_rad);
          normalized.localization_valid = raw->location == 0 && raw->map_resolution_m > 0.0;
          if (std::isfinite(raw->z_m)) normalized.z_m = raw->z_m;
          if (std::isfinite(raw->roll_rad)) normalized.roll_rad = raw->roll_rad;
          if (std::isfinite(raw->pitch_rad)) normalized.pitch_rad = raw->pitch_rad;
          if (raw->battery_percent >= 0 && raw->battery_percent <= 100) {
            normalized.battery_percent = raw->battery_percent;
          }
          if (std::isfinite(raw->speed_m_s)) normalized.forward_speed_m_s = raw->speed_m_s;
          if (std::isfinite(raw->current_odometry_km) && raw->current_odometry_km >= 0.0) {
            normalized.current_odometry_m = raw->current_odometry_km * 1000.0;
          }
          if (std::isfinite(raw->total_odometry_km) && raw->total_odometry_km >= 0.0) {
            normalized.total_odometry_m = raw->total_odometry_km * 1000.0;
          }
          if (std::isfinite(raw->map_resolution_m) && raw->map_resolution_m > 0.0) {
            normalized.map_resolution_m_per_pixel = raw->map_resolution_m;
          }
          if (std::isfinite(raw->map_origin_x_m)) {
            normalized.map_origin_x_m = raw->map_origin_x_m;
          }
          if (std::isfinite(raw->map_origin_y_m)) {
            normalized.map_origin_y_m = raw->map_origin_y_m;
          }
          if (std::isfinite(raw->map_height_pixels) && raw->map_height_pixels >= 0.0 &&
              raw->map_height_pixels <=
                  static_cast<double>(std::numeric_limits<std::int64_t>::max()) &&
              std::floor(raw->map_height_pixels) == raw->map_height_pixels) {
            normalized.map_height_pixels = static_cast<std::int64_t>(raw->map_height_pixels);
          }
          normalized.diagnostics = {
              {"motion_state", raw->motion_state},
              {"location", raw->location},
              {"rtk_state", raw->rtk_state},
              {"dock_state", raw->dock_state},
              {"gait_state", raw->gait_state},
              {"motor_state", raw->motor_state},
              {"charge_state", raw->charge_state},
              {"control_mode", raw->control_mode},
              {"map_update_state", raw->map_update_state},
              {"current_runtime", raw->current_runtime},
              {"total_runtime", raw->total_runtime},
          };
          sample = std::move(normalized);
        }
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        if (sample.has_value()) {
          impl_->status = std::move(sample);
          impl_->status_observed_steady = std::chrono::steady_clock::now();
          impl_->status_failed = false;
        }
      } catch (const std::exception& error) {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status_failed = true;
        eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_status_poll_failed",
                           {{"device", impl_->identity.device_id}, {"reason", error.what()}});
      } catch (...) {
        std::lock_guard<std::mutex> lock(impl_->status_mutex);
        impl_->status_failed = true;
        eos::lib::LogEvent(
            VendorLogger(), spdlog::level::warn, "vendor_status_poll_failed",
            {{"device", impl_->identity.device_id}, {"reason", "unknown_exception"}});
      }
      std::unique_lock<std::mutex> lock(impl_->status_mutex);
      if (impl_->status_wakeup.wait_for(lock, impl_->status_poll_interval,
                                        [this] { return impl_->stop_status; })) {
        return;
      }
    }
  });
  impl_->stream_worker = std::thread([this] {
    for (;;) {
      std::unique_lock<std::mutex> lock(impl_->mutex);
      const auto call_driver = [&](auto&& action) {
        lock.unlock();
        try {
          std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
          action();
        } catch (...) {
          lock.lock();
          throw;
        }
        lock.lock();
      };
      if (impl_->stream_wakeup.wait_for(lock, std::chrono::milliseconds(20),
                                        [this] { return impl_->stop_stream_worker; }))
        return;
      if (!impl_->stream_active && !impl_->stream_session_owned &&
          !impl_->stream_cleanup_pending) continue;
      try {
        const auto now = std::chrono::steady_clock::now();
        if (impl_->stream_cleanup_pending && now < impl_->stream_retry_at) continue;
        if (impl_->stream_cleanup_pending) {
          call_driver([&] {
            impl_->driver->CancelNavigationTarget();
            impl_->driver->WaitForNavigationIdle();
          });
          impl_->stream_session_owned = false;
          impl_->stream_cleanup_pending = false;
          if (!impl_->stream_active) continue;
        }
        const auto pose = ReadBaseTelemetry();
        if (!pose.has_value() || !pose->valid || !pose->fresh || !pose->pose_valid ||
            !pose->localization_valid) {
          const auto observed_update = impl_->stream_updated;
          impl_->stream_failure_reason = "stream navigation lost fresh localization";
          call_driver([&] {
            impl_->driver->CancelNavigationTarget();
            impl_->driver->WaitForNavigationIdle();
          });
          impl_->stream_session_owned = false;
          impl_->stream_cleanup_pending = false;
          if (impl_->stream_updated == observed_update) impl_->stream_active = false;
          continue;
        }
        if (now - impl_->stream_updated >= impl_->stream_idle_session_timeout) {
          call_driver([&] {
            impl_->driver->CancelNavigationTarget();
            impl_->driver->WaitForNavigationIdle();
          });
          impl_->stream_session_owned = false;
          impl_->stream_cleanup_pending = false;
          if (std::chrono::steady_clock::now() - impl_->stream_updated >=
              impl_->stream_idle_session_timeout) {
            impl_->stream_active = false;
            impl_->stream_holding = false;
          }
          continue;
        }
        if (!impl_->stream_holding &&
            now - impl_->stream_updated >= impl_->stream_inactivity_timeout) {
          impl_->stream_target = {pose->x_m, pose->y_m, pose->yaw_rad};
          impl_->stream_holding = true;
        }
        const double requested_z_m = impl_->stream_holding ? pose->z_m.value_or(0.0)
                                                            : impl_->stream_z_m.value_or(
                                                                  pose->z_m.value_or(0.0));
        const bool target_changed = !impl_->stream_session_owned ||
                                    impl_->stream_target != impl_->stream_submitted_target ||
                                    requested_z_m != impl_->stream_submitted_z_m ||
                                    impl_->stream_speed != impl_->stream_submitted_speed;
        if (impl_->stream_session_owned) {
          std::optional<int> outcome;
          call_driver([&] { outcome = impl_->driver->NavigationTargetOutcome(); });
          if (outcome && *outcome != 0 && !target_changed) {
            impl_->stream_failure_reason = "RobotServer navigation result " +
                                           std::to_string(*outcome);
            impl_->stream_active = false;
            impl_->stream_cleanup_pending = true;
            continue;
          }
          if (!target_changed) continue;
        }
        if (now < impl_->stream_retry_at) continue;
        double target_z_m = requested_z_m;
        if (impl_->stream_session_owned) {
          call_driver([&] {
            impl_->driver->CancelNavigationTarget();
            impl_->driver->WaitForNavigationIdle();
          });
          impl_->stream_session_owned = false;
          if (impl_->stream_holding && impl_->stream_active) {
            // The base may move while Type 1004 settles. A target sampled
            // before cancellation could drive it backwards after settlement.
            const auto settled_at = std::chrono::steady_clock::now();
            const auto pose_deadline = settled_at + impl_->status_poll_interval * 3;
            while (impl_->stream_holding && impl_->stream_active) {
              bool observed_after_settlement = false;
              {
                std::lock_guard<std::mutex> status_lock(impl_->status_mutex);
                observed_after_settlement = impl_->status_observed_steady > settled_at;
              }
              if (observed_after_settlement) {
                const auto settled_pose = ReadBaseTelemetry();
                if (settled_pose && settled_pose->valid && settled_pose->fresh &&
                    settled_pose->pose_valid && settled_pose->localization_valid) {
                  impl_->stream_target = {settled_pose->x_m, settled_pose->y_m,
                                          settled_pose->yaw_rad};
                  target_z_m = settled_pose->z_m.value_or(0.0);
                } else {
                  impl_->stream_failure_reason = "stream hold requires fresh localization";
                  impl_->stream_active = false;
                }
                break;
              }
              if (std::chrono::steady_clock::now() >= pose_deadline) {
                impl_->stream_failure_reason = "stream hold timed out waiting for localization";
                impl_->stream_active = false;
                break;
              }
              impl_->stream_wakeup.wait_for(lock, std::chrono::milliseconds(10));
              if (impl_->stop_stream_worker) return;
            }
          }
        }
        try {
          // Requests may arrive while cancellation settles. Submit only the
          // latest target after reacquiring the worker state lock.
          if (!impl_->stream_active) continue;
          const auto submitted_target = impl_->stream_target;
          const int submitted_speed = impl_->stream_speed;
          call_driver([&] { impl_->driver->StartNavigationTargetWithSpeed(
                                  submitted_target, target_z_m, submitted_speed); });
          impl_->stream_submitted_target = submitted_target;
          impl_->stream_submitted_z_m = target_z_m;
          impl_->stream_submitted_speed = submitted_speed;
          impl_->stream_session_owned = true;
          impl_->stream_cleanup_pending = false;
          impl_->stream_first_failure_at = {};
          impl_->stream_failure_reason.clear();
        } catch (const std::exception& error) {
          // Transient RobotServer unavailability must not cause overlapping
          // task sends. Retry only the latest target.
          impl_->stream_retry_at = now + std::chrono::milliseconds(100);
          if (impl_->stream_first_failure_at == std::chrono::steady_clock::time_point{}) {
            impl_->stream_first_failure_at = now;
          } else if (now - impl_->stream_first_failure_at >= std::chrono::seconds(1)) {
            impl_->stream_failure_reason = error.what();
            impl_->stream_active = false;
            impl_->stream_cleanup_pending = true;
          }
          eos::lib::LogEvent(VendorLogger(), spdlog::level::warn,
                             "vendor_stream_navigation_retry",
                             {{"device", impl_->identity.device_id}, {"reason", error.what()}});
        }
      } catch (const NavigationSettlingTimeout& error) {
        impl_->stream_cleanup_pending = true;
        impl_->stream_retry_at = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(500);
        eos::lib::LogEvent(VendorLogger(), spdlog::level::warn,
                           "vendor_stream_navigation_settling",
                           {{"device", impl_->identity.device_id}, {"reason", error.what()}});
      } catch (const std::exception& error) {
        impl_->stream_active = false;
        try {
          call_driver([&] { impl_->driver->CancelNavigationTarget(); });
        } catch (...) {}
        impl_->stream_session_owned = false;
        impl_->alive = false;
        eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_stream_failed",
                           {{"device", impl_->identity.device_id}, {"reason", error.what()}});
      } catch (...) {
        impl_->stream_active = false;
        try {
          call_driver([&] { impl_->driver->CancelNavigationTarget(); });
        } catch (...) {}
        impl_->stream_session_owned = false;
        impl_->alive = false;
      }
    }
  });
  eos::lib::LogEvent(VendorLogger(), spdlog::level::info, "vendor_backend_started",
                     {{"backend", "vendor-base"}, {"device", impl_->identity.device_id}});
}

VendorBase::~VendorBase() {
  const std::string device_id = impl_->identity.device_id;
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stop_stream_worker = true;
    impl_->stream_active = false;
  }
  impl_->stream_wakeup.notify_all();
  if (impl_->stream_worker.joinable()) impl_->stream_worker.join();
  try { impl_->driver->CancelNavigationTarget(); } catch (...) {}
  {
    std::lock_guard<std::mutex> lock(impl_->status_mutex);
    impl_->stop_status = true;
  }
  impl_->status_wakeup.notify_all();
  if (impl_->status_worker.joinable()) impl_->status_worker.join();
  impl_->driver.reset();
  eos::lib::LogEvent(VendorLogger(), spdlog::level::info, "vendor_backend_stopped",
                     {{"backend", "vendor-base"}, {"device", device_id}});
}

hardware::BackendIdentity VendorBase::Identify() const { return impl_->identity; }

hardware::FeedDeclarations VendorBase::DeclareFeeds() const {
  return {.geometric = {.max_rate_hz = 0.0},
          .proprioception = {.max_rate_hz = 2.0},
          .sensor = {.max_rate_hz = 0.0},
          .health = {.max_rate_hz = 10.0}};
}

hardware::BackendHealth VendorBase::ReadHealth() const {
  return {.alive = impl_->alive.load(), .admitted = IsAdmitted()};
}

std::optional<hardware::BaseTelemetry> VendorBase::ReadBaseTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->status_mutex);
  if (!impl_->status.has_value()) return std::nullopt;
  hardware::BaseTelemetry result = *impl_->status;
  if (impl_->status_failed) {
    result.valid = false;
    result.fresh = false;
  } else if (impl_->status_observed_steady != std::chrono::steady_clock::time_point{}) {
    result.fresh = std::chrono::steady_clock::now() - impl_->status_observed_steady <=
                   impl_->status_poll_interval * 3;
  }
  return result;
}

bool VendorBase::SupportsObstacleSnapshot() const { return impl_->driver->SupportsObstacles(); }

std::optional<hardware::ObstacleSnapshot> VendorBase::ReadObstacleSnapshot() const {
  if (!SupportsObstacleSnapshot()) return std::nullopt;
  try {
    return impl_->driver->ReadObstacles();
  } catch (...) {
    hardware::ObstacleSnapshot result;
    result.state = hardware::ObstacleState::kTransportError;
    return result;
  }
}

hardware::ReleaseResult VendorBase::ExecuteNavigateRelative(
    const hardware::BaseNavigateRelativeCommand& command) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->halted) {
    impl_->release_failure_reason = "vendor base is halted";
    return hardware::ReleaseResult::kRejected;
  }
  if (!impl_->alive) {
    impl_->release_failure_reason = "vendor base is unavailable";
    return hardware::ReleaseResult::kRejected;
  }
  constexpr double kTwoPi = 6.28318530717958647692;
  if (!std::isfinite(command.x_m) || !std::isfinite(command.y_m) ||
      !std::isfinite(command.yaw_rad) || std::abs(command.x_m) > 10.0 ||
      std::abs(command.y_m) > 10.0 || std::abs(command.yaw_rad) > kTwoPi) {
    impl_->release_failure_reason = "relative navigation target is outside vendor limits";
    return hardware::ReleaseResult::kRejected;
  }
  if ((command.z_m && !std::isfinite(*command.z_m)) ||
      (command.speed && (*command.speed != 0.0 && *command.speed != 1.0 &&
                         *command.speed != 2.0))) {
    impl_->release_failure_reason = "navigation height or speed is invalid";
    return hardware::ReleaseResult::kRejected;
  }
  if ((command.gait && *command.gait != 0 && *command.gait != 4) ||
      (command.stream && command.gait.value_or(0) == 4)) {
    impl_->release_failure_reason = "navigation gait is invalid for this command";
    return hardware::ReleaseResult::kRejected;
  }
  if (command.absolute) {
    if (!command.stream_map_target) {
      impl_->release_failure_reason = "absolute navigation requires a map target";
      return hardware::ReleaseResult::kRejected;
    }
    for (double value : *command.stream_map_target) {
      if (!std::isfinite(value)) {
        impl_->release_failure_reason = "absolute navigation target is invalid";
        return hardware::ReleaseResult::kRejected;
      }
    }
  }
  try {
    if (command.stream) {
      if (impl_->stream_cleanup_pending) {
        impl_->release_failure_reason = "previous stream navigation is settling";
        return hardware::ReleaseResult::kRejected;
      }
      if (!command.stream_map_target.has_value()) {
        impl_->release_failure_reason = "stream navigation requires a map target";
        return hardware::ReleaseResult::kRejected;
      }
      const auto pose = ReadBaseTelemetry();
      if (!pose.has_value() || !pose->valid || !pose->fresh || !pose->pose_valid ||
          !pose->localization_valid) {
        impl_->release_failure_reason = "stream navigation requires fresh localization";
        return hardware::ReleaseResult::kRejected;
      }
      for (double value : *command.stream_map_target) {
        if (!std::isfinite(value)) {
          impl_->release_failure_reason = "stream navigation target is invalid";
          return hardware::ReleaseResult::kRejected;
        }
      }
      impl_->stream_target = *command.stream_map_target;
      impl_->stream_z_m = command.z_m;
      impl_->stream_speed = static_cast<int>(command.speed.value_or(1.0));
      impl_->stream_updated = std::chrono::steady_clock::now();
      impl_->stream_retry_at = {};
      impl_->stream_active = true;
      impl_->stream_holding = false;
      impl_->stream_failure_reason.clear();
      impl_->stream_wakeup.notify_one();
      impl_->release_failure_reason.clear();
      return hardware::ReleaseResult::kAccepted;
    }
    std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
    impl_->stream_active = false;
    impl_->stream_cleanup_pending = false;
    impl_->driver->CancelNavigationTarget();
    impl_->stream_session_owned = false;
    impl_->driver->PrepareNavigation();
    {
      std::lock_guard<std::mutex> command_lock(impl_->navigation_command_mutex);
      impl_->active_navigation_command_id = command.command_id;
    }
    struct ClearNavigationCommand {
      Impl& impl;
      ~ClearNavigationCommand() {
        std::lock_guard<std::mutex> lock(impl.navigation_command_mutex);
        impl.active_navigation_command_id.clear();
      }
    } clear_navigation_command{*impl_};
    hardware::ReleaseResult result;
    if (command.absolute) {
      double z_m = command.z_m.value_or(0.0);
      if (!command.z_m) {
        const auto pose = ReadBaseTelemetry();
        if (!pose || !pose->valid || !pose->fresh || !pose->pose_valid ||
            !pose->localization_valid || !pose->z_m || !std::isfinite(*pose->z_m)) {
          impl_->release_failure_reason = "absolute navigation requires fresh localized height";
          return hardware::ReleaseResult::kRejected;
        }
        z_m = *pose->z_m;
      }
      const auto& map_target = *command.stream_map_target;
      result = impl_->driver->NavigateAbsoluteWithGait(
          map_target[0], map_target[1], z_m, map_target[2],
          static_cast<int>(command.speed.value_or(1.0)), command.gait.value_or(0));
    } else {
      result = impl_->driver->NavigateRelativeWithGait(command.x_m, command.y_m,
                                                      command.yaw_rad, command.gait.value_or(0));
    }
    impl_->release_failure_reason = result == hardware::ReleaseResult::kAccepted
                                        ? ""
                                        : command.absolute
                                              ? "vendor base refused the absolute navigation goal"
                                              : "vendor base refused the relative navigation goal";
    if (result == hardware::ReleaseResult::kAccepted) impl_->stream_failure_reason.clear();
    return result;
  } catch (const RobotServerNavigationRejection& error) {
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_navigation_rejected",
                       {{"operation", "navigate_relative"},
                        {"device", impl_->identity.device_id},
                        {"error_code", error.error_code},
                        {"error_status", error.error_status}},
                       error.what());
    return hardware::ReleaseResult::kRejected;
  } catch (const NavigationSettlingTimeout& error) {
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_navigation_settling",
                       {{"operation", "navigate_relative"},
                        {"device", impl_->identity.device_id}});
    return hardware::ReleaseResult::kRejected;
  } catch (const std::exception& error) {
    // Navigation preflight errors such as unavailable localization are
    // operational rejections; they must not disable independent posture calls.
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_navigation_rejected",
                       {{"operation", "navigate_relative"},
                        {"device", impl_->identity.device_id},
                        {"reason", error.what()}});
    return hardware::ReleaseResult::kRejected;
  } catch (...) {
    impl_->release_failure_reason = "unknown navigation failure";
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_navigation_rejected",
                       {{"operation", "navigate_relative"},
                        {"device", impl_->identity.device_id},
                        {"reason", "unknown_exception"}});
    return hardware::ReleaseResult::kRejected;
  }
}

hardware::ReleaseResult VendorBase::ExecuteGait(const hardware::BaseGaitCommand& command) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->halted || !impl_->alive || impl_->stream_active || impl_->stream_cleanup_pending) {
    impl_->release_failure_reason = "standalone gait switch requires an available idle base";
    return hardware::ReleaseResult::kRejected;
  }
  std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
  try {
    impl_->driver->SetGait(command.gait);
    impl_->release_failure_reason.clear();
    return hardware::ReleaseResult::kAccepted;
  } catch (const std::exception& error) {
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_gait_rejected",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
    return hardware::ReleaseResult::kRejected;
  }
}

hardware::ReleaseResult VendorBase::CancelCommand(const std::string& command_id) {
  if (command_id.empty()) return hardware::ReleaseResult::kRejected;
  std::lock_guard<std::mutex> lock(impl_->navigation_command_mutex);
  if (impl_->active_navigation_command_id != command_id) {
    return hardware::ReleaseResult::kRejected;
  }
  try {
    return impl_->driver->CancelNavigation();
  } catch (const std::exception& error) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_navigation_cancel_failed",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
    return hardware::ReleaseResult::kRejected;
  }
}

hardware::ReleaseResult VendorBase::ExecutePosture(const hardware::BasePostureCommand& command) {
  const hardware::BasePosture posture = command.posture;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
  if (impl_->halted) {
    impl_->release_failure_reason = "vendor base is halted";
    return hardware::ReleaseResult::kRejected;
  }
  if (!impl_->alive) {
    impl_->release_failure_reason = "vendor base is unavailable";
    return hardware::ReleaseResult::kRejected;
  }
  if (posture != hardware::BasePosture::kStand && posture != hardware::BasePosture::kWalk &&
      posture != hardware::BasePosture::kLie) {
    impl_->release_failure_reason = "unsupported vendor base posture";
    return hardware::ReleaseResult::kRejected;
  }
  try {
    impl_->stream_active = false;
    impl_->stream_cleanup_pending = false;
    impl_->driver->CancelNavigationTarget();
    impl_->stream_session_owned = false;
    switch (posture) {
      case hardware::BasePosture::kStand:
        impl_->driver->Stand();
        break;
      case hardware::BasePosture::kWalk:
        impl_->driver->Walk();
        break;
      case hardware::BasePosture::kLie:
        impl_->driver->Lie();
        break;
    }
    impl_->release_failure_reason.clear();
    impl_->stream_failure_reason.clear();
    return hardware::ReleaseResult::kAccepted;
  } catch (const NavigationSettlingTimeout& error) {
    impl_->stream_cleanup_pending = true;
    impl_->stream_retry_at = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(500);
    impl_->stream_wakeup.notify_one();
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_navigation_settling",
                       {{"operation", PostureName(posture)},
                        {"device", impl_->identity.device_id}});
    return hardware::ReleaseResult::kRejected;
  } catch (const std::exception& error) {
    // Failure to enter manual stepping is an operational rejection. Keep
    // standing and stop commands available so the operator can recover.
    if (posture != hardware::BasePosture::kWalk) impl_->alive = false;
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_driver_failed",
                       {{"operation", PostureName(posture)},
                        {"device", impl_->identity.device_id},
                        {"reason", error.what()}});
    return hardware::ReleaseResult::kRejected;
  } catch (...) {
    if (posture != hardware::BasePosture::kWalk) impl_->alive = false;
    impl_->release_failure_reason = "unknown posture failure";
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_driver_failed",
                       {{"operation", PostureName(posture)},
                        {"device", impl_->identity.device_id},
                        {"reason", "unknown_exception"}});
    return hardware::ReleaseResult::kRejected;
  }
}

hardware::ReleaseResult VendorBase::StopMotion() {
  std::string emergency_error;
  try {
    impl_->driver->EmergencySignal();
  } catch (const std::exception& error) {
    emergency_error = error.what();
  } catch (...) {
    emergency_error = "unknown emergency signal failure";
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
  if (impl_->halted) {
    impl_->release_failure_reason = "vendor base is halted";
    return hardware::ReleaseResult::kRejected;
  }
  try {
    // A navigation send may have raced the first signal while the worker held
    // the driver. Reassert estop after that send has finished.
    impl_->driver->EmergencySignal();
    emergency_error.clear();
    impl_->stream_active = false;
    impl_->stream_cleanup_pending = false;
    impl_->stream_session_owned = false;
    impl_->driver->SoftEmergencyStop();
    if (!emergency_error.empty()) throw std::runtime_error(emergency_error);
    impl_->release_failure_reason.clear();
    return hardware::ReleaseResult::kAccepted;
  } catch (const std::exception& error) {
    impl_->alive = false;
    impl_->release_failure_reason = error.what();
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_driver_failed",
                       {{"operation", "stop_motion"},
                        {"device", impl_->identity.device_id},
                        {"reason", error.what()}});
  } catch (...) {
    impl_->alive = false;
    impl_->release_failure_reason = "unknown stop failure";
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_driver_failed",
                       {{"operation", "stop_motion"},
                        {"device", impl_->identity.device_id},
                        {"reason", "unknown_exception"}});
  }
  return hardware::ReleaseResult::kRejected;
}

std::string VendorBase::ReleaseFailureReason() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->release_failure_reason.empty() ? "vendor base refused the release"
                                               : impl_->release_failure_reason;
}

void VendorBase::Clamp() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
  if (impl_->halted) return;
  impl_->stream_active = false;
  impl_->stream_cleanup_pending = false;
  try {
    impl_->driver->CancelNavigationTarget();
    impl_->driver->Clamp();
    impl_->stream_session_owned = false;
  } catch (const std::exception& error) {
    impl_->alive = false;
    eos::lib::LogEvent(
        VendorLogger(), spdlog::level::err, "vendor_driver_failed",
        {{"operation", "clamp"}, {"device", impl_->identity.device_id}, {"reason", error.what()}});
  } catch (...) {
    impl_->alive = false;
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_driver_failed",
                       {{"operation", "clamp"},
                        {"device", impl_->identity.device_id},
                        {"reason", "unknown_exception"}});
  }
}

void VendorBase::Halt() {
  try {
    impl_->driver->EmergencySignal();
  } catch (const std::exception& error) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_emergency_signal_failed",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
  } catch (...) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_emergency_signal_failed",
                       {{"device", impl_->identity.device_id}, {"reason", "unknown_exception"}});
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  std::lock_guard<std::mutex> driver_lock(impl_->driver_mutex);
  if (impl_->halted) return;
  try {
    // Close the window where a queued navigation send followed the first signal.
    impl_->driver->EmergencySignal();
  } catch (const std::exception& error) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_emergency_signal_failed",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
  }
  impl_->halted = true;
  impl_->stream_active = false;
  impl_->stream_cleanup_pending = false;
  impl_->stream_session_owned = false;
  std::string error;
  try {
    impl_->driver->Halt();
  } catch (const std::exception& caught) {
    error = caught.what();
  } catch (...) {
    error = "unknown_exception";
  }
  impl_->alive = false;
  if (!error.empty()) {
    eos::lib::LogEvent(
        VendorLogger(), spdlog::level::err, "vendor_driver_failed",
        {{"operation", "halt"}, {"device", impl_->identity.device_id}, {"reason", error}});
  }
  eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "vendor_backend_halted",
                     {{"device", impl_->identity.device_id}});
}

}  // namespace eos::backends::vendor

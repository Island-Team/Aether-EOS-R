#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "rust/cxx.h"

namespace eos::hal {
class HardwareBase;
class HardwareCamera;
class HardwareManipulator;
struct BaseReading;
struct CameraReading;
struct ManipulatorReading;
}  // namespace eos::hal

namespace eos::hal {

class HardwareBase {
 public:
  BaseReading read_base() const;
  bool submit_body_velocity(double x_m_s, double y_m_s, double yaw_rad_s,
                            std::uint64_t valid_for_ms) const;
  bool stop() const;
  bool is_stopped() const;
  void set_mock_submit_delay_ms(std::uint32_t delay_ms) const;
  bool mock_submit_started() const;

 private:
  mutable std::mutex state_mutex_;
  mutable double x_m_s_ = 0.0;
  mutable double y_m_s_ = 0.0;
  mutable double yaw_rad_s_ = 0.0;
  mutable std::chrono::steady_clock::time_point expires_at_{};
  mutable std::atomic<bool> stopped_{false};
  mutable std::atomic<std::uint32_t> mock_submit_delay_ms_{0};
  mutable std::atomic<bool> mock_submit_started_{false};
};

std::shared_ptr<HardwareBase> new_mock_base();

class HardwareCamera {
 public:
  CameraReading read_camera() const;
};

std::unique_ptr<HardwareCamera> new_mock_camera();

class HardwareManipulator {
 public:
  explicit HardwareManipulator(std::uint32_t joint_count);
  ManipulatorReading read_manipulator() const;
  bool submit_joint_positions(rust::Slice<const double> positions, bool has_max_speed,
                              double max_speed_rad_s) const;
  bool submit_end_effector_pose(rust::Slice<const double> position,
                                rust::Slice<const double> orientation_xyzw, bool has_max_speed,
                                double max_speed_m_s) const;
  bool stop() const;
  bool is_stopped() const;
  void set_mock_submit_delay_ms(std::uint32_t delay_ms) const;
  bool mock_submit_started() const;

 private:
  const std::size_t joint_count_;
  mutable std::mutex state_mutex_;
  mutable std::vector<double> positions_;
  mutable std::vector<double> end_position_ = {0.0, 0.0, 0.0};
  mutable std::vector<double> end_orientation_ = {0.0, 0.0, 0.0, 1.0};
  mutable std::atomic<bool> stopped_{false};
  mutable std::atomic<std::uint32_t> mock_submit_delay_ms_{0};
  mutable std::atomic<bool> mock_submit_started_{false};
};

std::shared_ptr<HardwareManipulator> new_mock_manipulator(std::uint32_t joint_count);

}  // namespace eos::hal

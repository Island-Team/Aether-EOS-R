#include "mock_hardware.h"

#include <cmath>
#include <limits>
#include <thread>

#include "aether-hal/src/cxx_bridge.rs.h"

namespace eos::hal {

BaseReading HardwareBase::read_base() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (stopped_.load(std::memory_order_acquire)) {
    return BaseReading{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true};
  }
  if (std::chrono::steady_clock::now() >= expires_at_) {
    return BaseReading{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, true};
  }
  return BaseReading{0.0, 0.0, 0.0, x_m_s_, y_m_s_, yaw_rad_s_, true};
}

bool HardwareBase::submit_body_velocity(double x_m_s, double y_m_s, double yaw_rad_s,
                                        std::uint64_t valid_for_ms) const {
  if (!std::isfinite(x_m_s) || !std::isfinite(y_m_s) || !std::isfinite(yaw_rad_s) ||
      valid_for_ms == 0) {
    return false;
  }
  if (valid_for_ms > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  mock_submit_started_.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(
      mock_submit_delay_ms_.load(std::memory_order_relaxed)));
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (stopped_.load(std::memory_order_acquire)) return false;
  x_m_s_ = x_m_s;
  y_m_s_ = y_m_s;
  yaw_rad_s_ = yaw_rad_s;
  expires_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(valid_for_ms);
  return true;
}

bool HardwareBase::stop() const {
  // The latch is independent of state_mutex_, so a delayed submit cannot
  // prevent the stop request from taking effect or later re-enable motion.
  stopped_.store(true, std::memory_order_release);
  return true;
}

bool HardwareBase::is_stopped() const {
  return stopped_.load(std::memory_order_acquire);
}

void HardwareBase::set_mock_submit_delay_ms(std::uint32_t delay_ms) const {
  mock_submit_delay_ms_.store(delay_ms, std::memory_order_relaxed);
}

bool HardwareBase::mock_submit_started() const {
  return mock_submit_started_.load(std::memory_order_acquire);
}

std::shared_ptr<HardwareBase> new_mock_base() { return std::make_shared<HardwareBase>(); }

CameraReading HardwareCamera::read_camera() const {
  CameraReading reading{};
  reading.width_px = 2;
  reading.height_px = 2;
  reading.stride_bytes = 6;
  for (std::uint8_t value : {0, 16, 32, 48, 64, 80, 96, 112, 128, 144, 160, 176}) {
    reading.rgb8.push_back(value);
  }
  for (std::uint16_t depth : {1000, 1100, 1200, 1300}) {
    reading.depth_u16.push_back(depth);
  }
  reading.depth_scale_m = 0.001;
  reading.fx = 1.0;
  reading.fy = 1.0;
  reading.cx = 0.5;
  reading.cy = 0.5;
  reading.has_depth = true;
  return reading;
}

std::unique_ptr<HardwareCamera> new_mock_camera() { return std::make_unique<HardwareCamera>(); }

HardwareManipulator::HardwareManipulator(std::uint32_t joint_count)
    : joint_count_(joint_count), positions_(joint_count, 0.0) {}

ManipulatorReading HardwareManipulator::read_manipulator() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ManipulatorReading reading{};
  for (double position : positions_) {
    reading.joint_positions_rad.push_back(position);
    reading.joint_velocities_rad_s.push_back(0.0);
  }
  for (double component : end_position_) {
    reading.end_position_m.push_back(component);
  }
  for (double component : end_orientation_) {
    reading.end_orientation_xyzw.push_back(component);
  }
  reading.valid = true;
  return reading;
}

bool HardwareManipulator::submit_joint_positions(rust::Slice<const double> positions,
                                                 bool has_max_speed, double max_speed_rad_s) const {
  if (positions.size() != joint_count_ ||
      (has_max_speed && (!std::isfinite(max_speed_rad_s) || max_speed_rad_s <= 0.0))) {
    return false;
  }
  for (double value : positions) {
    if (!std::isfinite(value)) return false;
  }
  mock_submit_started_.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(
      mock_submit_delay_ms_.load(std::memory_order_relaxed)));
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (stopped_.load(std::memory_order_acquire)) return false;
  positions_.assign(positions.begin(), positions.end());
  return true;
}

bool HardwareManipulator::submit_end_effector_pose(rust::Slice<const double> position,
                                                   rust::Slice<const double> orientation_xyzw,
                                                   bool has_max_speed, double max_speed_m_s) const {
  if (position.size() != 3 || orientation_xyzw.size() != 4 ||
      (has_max_speed && (!std::isfinite(max_speed_m_s) || max_speed_m_s <= 0.0))) {
    return false;
  }
  for (double value : position) {
    if (!std::isfinite(value)) return false;
  }
  double qnorm = 0.0;
  for (double value : orientation_xyzw) {
    if (!std::isfinite(value)) return false;
    qnorm += value * value;
  }
  if (std::abs(qnorm - 1.0) > 0.001) return false;
  mock_submit_started_.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(
      mock_submit_delay_ms_.load(std::memory_order_relaxed)));
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (stopped_.load(std::memory_order_acquire)) return false;
  end_position_.assign(position.begin(), position.end());
  end_orientation_.assign(orientation_xyzw.begin(), orientation_xyzw.end());
  return true;
}

bool HardwareManipulator::stop() const {
  stopped_.store(true, std::memory_order_release);
  return true;
}

bool HardwareManipulator::is_stopped() const {
  return stopped_.load(std::memory_order_acquire);
}

void HardwareManipulator::set_mock_submit_delay_ms(std::uint32_t delay_ms) const {
  mock_submit_delay_ms_.store(delay_ms, std::memory_order_relaxed);
}

bool HardwareManipulator::mock_submit_started() const {
  return mock_submit_started_.load(std::memory_order_acquire);
}

std::shared_ptr<HardwareManipulator> new_mock_manipulator(std::uint32_t joint_count) {
  return std::make_shared<HardwareManipulator>(joint_count);
}

}  // namespace eos::hal

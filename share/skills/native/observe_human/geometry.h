#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>

namespace island::skills::native::detail {

inline constexpr double kHorizontalFovRad = 1.4;

inline double VerticalFovRad(double horizontal_fov_rad, double width, double height) {
  return 2.0 * std::atan(std::tan(horizontal_fov_rad / 2.0) * height / width);
}

inline double NormalizedImageErrorToAngle(double normalized_error, double fov_rad) {
  const double bounded_error = std::clamp(normalized_error, -1.0, 1.0);
  return std::atan(bounded_error * std::tan(fov_rad / 2.0));
}

struct FollowCorrection {
  double raw_angle_rad{0.0};
  double effective_angle_rad{0.0};
  double gain{0.0};
  bool capped{false};
};

struct CenteredState {
  bool centered{false};
  std::size_t consecutive_exit_samples{0};
};

inline CenteredState UpdateCenteredState(CenteredState state, double horizontal_error,
                                         double vertical_error, double horizontal_enter,
                                         double vertical_enter, double horizontal_exit,
                                         double vertical_exit) {
  const bool inside_enter = std::abs(horizontal_error) <= horizontal_enter &&
                            std::abs(vertical_error) <= vertical_enter;
  const bool outside_exit = std::abs(horizontal_error) > horizontal_exit ||
                            std::abs(vertical_error) > vertical_exit;
  if (!state.centered) {
    if (inside_enter) state.centered = true;
    state.consecutive_exit_samples = 0;
    return state;
  }
  state.consecutive_exit_samples = outside_exit ? state.consecutive_exit_samples + 1 : 0;
  if (state.consecutive_exit_samples >= 2) {
    state.centered = false;
    state.consecutive_exit_samples = 0;
  }
  return state;
}

inline FollowCorrection BoundedFollowCorrection(double normalized_error, double fov_rad,
                                                double deadband, double near_center_limit,
                                                double near_center_cap_rad) {
  const double bounded_error = std::clamp(normalized_error, -1.0, 1.0);
  const double raw = NormalizedImageErrorToAngle(bounded_error, fov_rad);
  const double deadband_angle = NormalizedImageErrorToAngle(deadband, fov_rad);
  const double excess = std::copysign(std::max(0.0, std::abs(raw) - deadband_angle), raw);
  if (excess == 0.0) return {raw, 0.0, 0.0, false};

  const double span = std::max(near_center_limit - deadband, 1e-9);
  const double progress = std::clamp((std::abs(bounded_error) - deadband) / span, 0.0, 1.0);
  const double gain = 0.5 + 0.5 * progress;
  double effective = excess * gain;
  bool capped = false;
  if (std::abs(bounded_error) <= near_center_limit &&
      std::abs(effective) > near_center_cap_rad) {
    effective = std::copysign(near_center_cap_rad, effective);
    capped = true;
  }
  return {raw, effective, gain, capped};
}

}  // namespace island::skills::native::detail

#include "auto_base.h"

#include <algorithm>
#include <cctype>

namespace eos::ext::repl {

AutoBaseKeys::Nudge AutoBaseKeys::Press(char key) const {
  const double scale = static_cast<double>(speed_percent_) / 100.0;
  switch (std::tolower(static_cast<unsigned char>(key))) {
    case 'w': return {.x_m = 0.24 * scale};
    case 's': return {.x_m = -0.24 * scale};
    case 'a': return {.y_m = 0.24 * scale};
    case 'd': return {.y_m = -0.24 * scale};
    case 'j': return {.yaw_rad = 0.45 * scale};
    case 'k': return {.yaw_rad = -0.45 * scale};
    default: return {};
  }
}

bool AutoBaseKeys::AdjustSpeed(char key) {
  if (key == '[') {
    speed_percent_ = std::max(25, speed_percent_ - 25);
    return true;
  }
  if (key == ']') {
    speed_percent_ = std::min(200, speed_percent_ + 25);
    return true;
  }
  return false;
}

}  // namespace eos::ext::repl

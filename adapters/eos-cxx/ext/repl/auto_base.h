#pragma once

namespace eos::ext::repl {

// Each terminal key event produces one relative navigation target. A held key
// produces further targets only when the terminal sends repeat events.
class AutoBaseKeys final {
 public:
  struct Nudge {
    double x_m = 0;
    double y_m = 0;
    double yaw_rad = 0;
    bool active() const { return x_m != 0 || y_m != 0 || yaw_rad != 0; }
  };

  Nudge Press(char key) const;
  bool AdjustSpeed(char key);
  int SpeedPercent() const { return speed_percent_; }

 private:
  int speed_percent_ = 100;
};

}  // namespace eos::ext::repl

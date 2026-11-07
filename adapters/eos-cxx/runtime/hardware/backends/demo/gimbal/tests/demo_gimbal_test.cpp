/// Structure test: drives stub data through the demo gimbal in the same
/// order the release chain does — admission, release, fail-safe primitives —
/// and reads the stub pan/tilt feed back. No runtime modules involved.
#include "demo_gimbal.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

/// True when `a` is within 1e-9 of `b`.
bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
  using namespace eos;
  using backends::demo::DemoGimbalState;

  backends::demo::DemoGimbal gimbal("demo-gimbal-0");
  const hardware::BackendIdentity id = gimbal.Identify();
  assert(id.name == "demo-gimbal");
  assert(id.kind == hardware::DeviceKind::kGimbal);
  assert(id.device_id == "demo-gimbal-0");
  const hardware::FeedDeclarations feeds = gimbal.DeclareFeeds();
  assert(feeds.proprioception.max_rate_hz == 10.0);
  assert(gimbal.ReadHealth().alive && !gimbal.ReadHealth().admitted);

  hardware::MoveLCommand cart{};
  assert(gimbal.ExecuteMoveL(cart) == hardware::ReleaseResult::kRejected);
  hardware::BaseCommand base{};
  assert(gimbal.ExecuteBase(base) == hardware::ReleaseResult::kRejected);

  hardware::GimbalCommand cmd{};
  cmd.pan_rad = 0.6;
  cmd.tilt_rad = 0.4;
  cmd.speed_rad_s = 0.5;
  assert(gimbal.ExecuteGimbal(cmd) == hardware::ReleaseResult::kAccepted);
  const DemoGimbalState s = gimbal.ReadProprioception();
  assert(Near(s.pan_rad, 0.6) && Near(s.tilt_rad, 0.4));
  assert(Near(s.pan_rate, 0.5));
  cmd.stream = true;
  cmd.pan_rad = 0.7;
  assert(gimbal.ExecuteGimbal(cmd) == hardware::ReleaseResult::kAccepted);
  assert(Near(gimbal.ReadProprioception().pan_rad, 0.7));
  const auto neutral = gimbal.ReadGimbalTelemetry();
  assert(neutral.has_value());
  assert(Near(neutral->pan_rad, s.pan_rad));
  hardware::GimbalCommand invalid = cmd;
  invalid.pan_rad = 6.29;
  assert(gimbal.ExecuteGimbal(invalid) == hardware::ReleaseResult::kRejected);
  invalid = cmd;
  invalid.speed_rad_s = 0.0;
  assert(gimbal.ExecuteGimbal(invalid) == hardware::ReleaseResult::kRejected);

  gimbal.Clamp();
  assert(gimbal.ReadProprioception().pan_rate == 0.0);
  assert(Near(gimbal.ReadProprioception().pan_rad, 0.7));
  gimbal.Halt();
  assert(gimbal.ExecuteGimbal(cmd) == hardware::ReleaseResult::kRejected);
  assert(!gimbal.ReadHealth().alive);

  std::puts("demo gimbal structure test: OK");
  return 0;
}

/// Structure test: drives stub data through the demo base in the same order
/// the release chain does — admission, release, fail-safe primitives — and
/// reads the stub odometry feed back. No runtime modules involved.
#include "demo_base.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
  using namespace eos;

  backends::demo::DemoBase base("demo-base-0");
  const hardware::BackendIdentity id = base.Identify();
  assert(id.name == "demo-base");
  assert(id.kind == hardware::DeviceKind::kBase);
  assert(id.device_id == "demo-base-0");
  const hardware::FeedDeclarations feeds = base.DeclareFeeds();
  assert(feeds.proprioception.max_rate_hz == 0.0);
  assert(base.ReadHealth().alive && !base.ReadHealth().admitted);

  hardware::MoveLCommand cart{};
  assert(base.ExecuteMoveL(cart) == hardware::ReleaseResult::kRejected);

  assert(base.ReadPosture() == hardware::BasePosture::kStand);
  assert(base.ExecutePosture({hardware::BasePosture::kWalk, "posture-walk"}) ==
         hardware::ReleaseResult::kAccepted);
  assert(base.ReadPosture() == hardware::BasePosture::kWalk);
  assert(base.ExecutePosture({static_cast<hardware::BasePosture>(255), "posture-invalid"}) ==
         hardware::ReleaseResult::kRejected);
  assert(base.ReadPosture() == hardware::BasePosture::kWalk);

  hardware::BaseCommand cmd{};
  cmd.linear_x_m_s = 0.5;
  cmd.angular_z_rad_s = 0.1;
  assert(base.ExecuteBase(cmd) == hardware::ReleaseResult::kAccepted);
  base.Tick(1.0);
  const backends::demo::DemoBaseOdometry odo = base.ReadProprioception();
  assert(std::fabs(odo.x - 0.5) < 1e-9);
  assert(std::fabs(odo.yaw - 0.1) < 1e-9);
  assert(odo.vx == 0.5);
  const auto neutral = base.ReadBaseTelemetry();
  assert(neutral.has_value());
  assert(std::fabs(neutral->x_m - odo.x) < 1e-9);
  assert(neutral->posture == hardware::BasePosture::kWalk);
  cmd.linear_x_m_s = 1.1;
  assert(base.ExecuteBase(cmd) == hardware::ReleaseResult::kRejected);
  cmd.linear_x_m_s = 0.5;

  hardware::BaseNavigateRelativeCommand navigation{};
  navigation.x_m = 0.2;
  navigation.y_m = -0.1;
  navigation.yaw_rad = 0.05;
  navigation.stream = true;
  assert(base.ExecuteNavigateRelative(navigation) == hardware::ReleaseResult::kAccepted);
  const backends::demo::DemoBaseOdometry navigated = base.ReadProprioception();

  base.Clamp();
  assert(base.ReadProprioception().vx == 0.0);
  assert(base.ReadProprioception().x == navigated.x);
  base.Halt();
  assert(base.ExecuteBase(cmd) == hardware::ReleaseResult::kRejected);
  assert(base.ExecutePosture({hardware::BasePosture::kLie, "posture-halted"}) ==
         hardware::ReleaseResult::kRejected);
  assert(base.ReadPosture() == hardware::BasePosture::kWalk);
  assert(!base.ReadHealth().alive);

  std::puts("demo base structure test: OK");
  return 0;
}

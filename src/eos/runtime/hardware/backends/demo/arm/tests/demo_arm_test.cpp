/// Structure test: drives stub data through the demo arm in the same order
/// the release chain does — admission, release, fail-safe primitives — and
/// reads the stub feeds back. No runtime modules involved.
#include "demo_arm.h"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
  using namespace eos;

  backends::demo::DemoArm arm("demo-arm-0");
  const hardware::BackendIdentity id = arm.Identify();
  assert(id.name == "demo-arm");
  assert(id.kind == hardware::DeviceKind::kManipulator);
  assert(id.device_id == "demo-arm-0");
  const hardware::FeedDeclarations feeds = arm.DeclareFeeds();
  assert(feeds.proprioception.max_rate_hz == 10.0);
  assert(arm.ReadHealth().alive && !arm.ReadHealth().admitted);

  hardware::MoveLCommand cmd{};
  cmd.position_m[0] = 0.4;
  cmd.position_m[1] = -0.1;
  cmd.position_m[2] = 0.25;
  assert(arm.ExecuteMoveL(cmd) == hardware::ReleaseResult::kAccepted);
  const backends::demo::DemoArmProprioception p = arm.ReadProprioception();
  assert(p.ee_position[0] == 0.4 && p.ee_position[2] == 0.25);
  assert(p.ee_linear_velocity[0] == 0.0);
  cmd.stream = true;
  cmd.position_m[0] = 0.35;
  assert(arm.ExecuteMoveL(cmd) == hardware::ReleaseResult::kAccepted);
  assert(arm.ReadProprioception().ee_position[0] == 0.35);

  hardware::BaseCommand base{};
  assert(arm.ExecuteBase(base) == hardware::ReleaseResult::kRejected);

  hardware::MoveLCommand unreachable{};
  unreachable.position_m[0] = 2.0;
  assert(arm.ExecuteMoveL(unreachable) == hardware::ReleaseResult::kRejected);

  hardware::MoveJCommand joint_cmd{};
  constexpr double kPi = 3.14159265358979323846;
  joint_cmd.joint_positions_rad = {-kPi, -0.6, -0.2, 0.2, 0.6, kPi};
  joint_cmd.speed_rad_s = 0.5;
  assert(arm.ExecuteMoveJ(joint_cmd) == hardware::ReleaseResult::kAccepted);
  assert(std::fabs(arm.ReadProprioception().joint_positions_rad[0] + kPi) < 1e-9);
  assert(std::fabs(arm.ReadProprioception().joint_positions_rad[5] - kPi) < 1e-9);
  joint_cmd.stream = true;
  joint_cmd.joint_positions_rad[3] = 0.3;
  assert(arm.ExecuteMoveJ(joint_cmd) == hardware::ReleaseResult::kAccepted);
  assert(std::fabs(arm.ReadProprioception().joint_positions_rad[3] - 0.3) < 1e-9);

  hardware::MoveJCommand partial{};
  partial.joint_positions_rad = {0.5};
  assert(arm.ExecuteMoveJ(partial) == hardware::ReleaseResult::kRejected);
  backends::demo::DemoArm two_joint_arm("demo-arm-1", 0.7, 0.5, 0.0, 0.0, 2);
  hardware::MoveJCommand two_joint_command{};
  two_joint_command.joint_positions_rad = {0.25, 0.75};
  assert(two_joint_arm.ExecuteMoveJ(two_joint_command) == hardware::ReleaseResult::kAccepted);
  hardware::MoveJCommand out_of_range = joint_cmd;
  out_of_range.joint_positions_rad[2] = 6.29;
  assert(arm.ExecuteMoveJ(out_of_range) == hardware::ReleaseResult::kRejected);

  hardware::PickCommand pick{.object_id = "obj:part-7"};
  assert(arm.ExecutePick(pick) == hardware::ReleaseResult::kAccepted);

  arm.Clamp();
  assert(arm.ReadProprioception().ee_linear_velocity[0] == 0.0);
  assert(arm.ReadProprioception().joint_velocities[0] == 0.0);
  assert(arm.ReadProprioception().ee_position[0] == 0.4);
  arm.Halt();
  assert(arm.ExecuteMoveL(cmd) == hardware::ReleaseResult::kRejected);
  assert(arm.ExecuteMoveJ(joint_cmd) == hardware::ReleaseResult::kRejected);
  assert(!arm.ReadHealth().alive);

  std::puts("demo arm structure test: OK");
  return 0;
}

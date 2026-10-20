/// Structure test: drives stub data through the demo gripper in the same
/// order the release chain does — admission, release (including the armed
/// grasp flow), fail-safe primitives — and reads the stub jaw feed back. No
/// runtime modules involved.
#include "demo_gripper.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  using namespace eos;
  using backends::demo::DemoGripperJaw;
  using backends::demo::DemoGripperState;

  backends::demo::DemoGripper gripper("demo-gripper-0");
  const hardware::BackendIdentity id = gripper.Identify();
  assert(id.name == "demo-gripper");
  assert(id.kind == hardware::DeviceKind::kGripper);
  assert(id.device_id == "demo-gripper-0");
  const hardware::FeedDeclarations feeds = gripper.DeclareFeeds();
  assert(feeds.proprioception.max_rate_hz == 10.0);
  assert(gripper.ReadHealth().alive && !gripper.ReadHealth().admitted);

  // Bare construction: open, nothing held, default commanded force 5 N.
  DemoGripperState s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kOpen);
  assert(s.object_held_id.empty());
  assert(s.force_n == 5.0);
  assert(s.position_pct == 100.0);

  // Wrong-kind releases are rejected.
  hardware::MoveLCommand cart{};
  assert(gripper.ExecuteMoveL(cart) == hardware::ReleaseResult::kRejected);
  hardware::BaseCommand base{};
  assert(gripper.ExecuteBase(base) == hardware::ReleaseResult::kRejected);

  // Plain close (no armed target): closed on nothing at the commanded force.
  hardware::GripperCommand cmd{};
  cmd.action = hardware::GripperAction::kClose;
  cmd.force_n = 8.0;
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kAccepted);
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kClosed);
  assert(s.object_held_id.empty());
  assert(s.force_n == 8.0);
  assert(s.position_pct == 0.0);
  hardware::GripperCommand invalid = cmd;
  invalid.force_n = 0.0;
  assert(gripper.ExecuteGripper(invalid) == hardware::ReleaseResult::kRejected);
  invalid = cmd;
  invalid.action = static_cast<hardware::GripperAction>(255);
  assert(gripper.ExecuteGripper(invalid) == hardware::ReleaseResult::kRejected);

  // Arm -> open (target survives) -> close consumes the target into holding.
  gripper.ArmGrasp("object-1");
  hardware::GripperCommand open_cmd{};
  open_cmd.action = hardware::GripperAction::kOpen;
  open_cmd.force_n = 6.0;
  assert(gripper.ExecuteGripper(open_cmd) == hardware::ReleaseResult::kAccepted);
  cmd.force_n = 6.5;
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kAccepted);
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kHolding);
  assert(s.object_held_id == "object-1");
  assert(s.force_n == 6.5);
  assert(s.position_pct == 0.0);

  // The next close without a fresh target closes on nothing again.
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kAccepted);
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kClosed);
  assert(s.object_held_id.empty());

  // Open releases a held object; a fresh arm re-grasps a new target.
  gripper.ArmGrasp("object-2");
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kAccepted);
  assert(gripper.ReadProprioception().object_held_id == "object-2");
  assert(gripper.ExecuteGripper(open_cmd) == hardware::ReleaseResult::kAccepted);
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kOpen);
  assert(s.object_held_id.empty());
  assert(s.position_pct == 100.0);

  // Clamp: hold the jaw state, stop applying force — no motion to zero.
  gripper.ArmGrasp("object-3");
  cmd.force_n = 4.0;
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kAccepted);
  gripper.Clamp();
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kHolding);
  assert(s.object_held_id == "object-3");
  assert(s.force_n == 0.0);
  assert(s.position_pct == 0.0);

  // Halt: clamp plus the latch — every further release is rejected.
  gripper.Halt();
  assert(gripper.ExecuteGripper(cmd) == hardware::ReleaseResult::kRejected);
  assert(gripper.ExecuteGripper(open_cmd) == hardware::ReleaseResult::kRejected);
  s = gripper.ReadProprioception();
  assert(s.state == DemoGripperJaw::kHolding);  // state survives the halt
  assert(!gripper.ReadHealth().alive);

  std::puts("demo gripper structure test: OK");
  return 0;
}

#include "demo_gripper.h"

#include <cmath>
#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoGripper::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-gripper",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kGripper},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 10.0},
                                .sensor = {.max_rate_hz = 0.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver) {}
};

DemoGripper::DemoGripper(std::string device_id)
    : DemoGripper(std::move(device_id), std::make_shared<ext::simulator::ipc::SimulatorClient>(
                                            "var/run/simulator.sock")) {}

DemoGripper::DemoGripper(std::string device_id,
                         std::shared_ptr<ext::simulator::ipc::SimulatorClient> world)
    : impl_(new Impl(std::move(device_id), std::move(world))) {}

DemoGripper::~DemoGripper() { delete impl_; }

hardware::BackendIdentity DemoGripper::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoGripper::DeclareFeeds() const { return impl_->core.feeds(); }

hardware::ReleaseResult DemoGripper::ExecuteGripper(const hardware::GripperCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if (cmd.action != hardware::GripperAction::kOpen &&
      cmd.action != hardware::GripperAction::kClose) {
    return hardware::ReleaseResult::kRejected;
  }
  if (cmd.force_n && (!std::isfinite(*cmd.force_n) || *cmd.force_n <= 0.0 || *cmd.force_n > 10.0)) {
    return hardware::ReleaseResult::kRejected;
  }
  constexpr double kDefaultForceN = 5.0;
  return impl_->driver->MoveGripper(impl_->core.identity().device_id,
                                    cmd.action == hardware::GripperAction::kClose,
                                    cmd.force_n.value_or(kDefaultForceN))
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

std::optional<hardware::GripperTelemetry> DemoGripper::ReadGripperTelemetry() const {
  const DemoGripperState state = ReadProprioception();
  hardware::GripperState semantic = hardware::GripperState::kUnknown;
  switch (state.state) {
    case DemoGripperJaw::kOpen:
      semantic = hardware::GripperState::kOpen;
      break;
    case DemoGripperJaw::kClosed:
      semantic = hardware::GripperState::kClosed;
      break;
    case DemoGripperJaw::kHolding:
      semantic = hardware::GripperState::kHolding;
      break;
  }
  return hardware::GripperTelemetry{
      .state = semantic,
      .force_n = state.force_n,
      .position_pct = state.position_pct,
  };
}

void DemoGripper::Clamp() {
  // A gripper's moving quantity is applied force, not position: the safe
  // envelope stops applying force (force -> 0) while holding the jaw state —
  // position, semantic state, and any held id stay so the composition layer
  // can still read what was grasped. Deliberately no spring-open to zero.
  impl_->driver->StopGripper(impl_->core.identity().device_id);
}

void DemoGripper::Halt() {
  Clamp();
  impl_->core.LatchHalt();
}

void DemoGripper::ArmGrasp(const std::string& object_id) {
  impl_->driver->ArmGrasp(impl_->core.identity().device_id, object_id);
}

DemoGripperState DemoGripper::ReadProprioception() const {
  return impl_->driver->ReadGripper(impl_->core.identity().device_id).value_or(DemoGripperState{});
}

DemoGripperHealth DemoGripper::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoGripperHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

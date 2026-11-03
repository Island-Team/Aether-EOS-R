#include "demo_gimbal.h"

#include <cmath>
#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoGimbal::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-gimbal",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kGimbal},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 10.0},
                                .sensor = {.max_rate_hz = 0.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver) {}
};

DemoGimbal::DemoGimbal(std::string device_id)
    : DemoGimbal(std::move(device_id),
                 std::make_shared<ext::simulator::ipc::SimulatorClient>("var/run/simulator.sock")) {
}

DemoGimbal::DemoGimbal(std::string device_id,
                       std::shared_ptr<ext::simulator::ipc::SimulatorClient> world)
    : impl_(new Impl(std::move(device_id), std::move(world))) {}

DemoGimbal::~DemoGimbal() { delete impl_; }

hardware::BackendIdentity DemoGimbal::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoGimbal::DeclareFeeds() const { return impl_->core.feeds(); }

hardware::ReleaseResult DemoGimbal::ExecuteGimbal(const hardware::GimbalCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  constexpr double kJointLimit = 6.28;
  if (!std::isfinite(cmd.pan_rad) || !std::isfinite(cmd.tilt_rad) || cmd.pan_rad < -kJointLimit ||
      cmd.pan_rad > kJointLimit || cmd.tilt_rad < -kJointLimit || cmd.tilt_rad > kJointLimit ||
      (cmd.speed_rad_s && (!std::isfinite(*cmd.speed_rad_s) || *cmd.speed_rad_s <= 0.0))) {
    return hardware::ReleaseResult::kRejected;
  }
  const double rate = cmd.speed_rad_s.value_or(0.0);
  return impl_->driver->PointGimbal(impl_->core.identity().device_id, cmd.pan_rad, cmd.tilt_rad,
                                    rate, rate)
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoGimbal::StopMotion() {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  Clamp();
  return hardware::ReleaseResult::kAccepted;
}

void DemoGimbal::Clamp() { impl_->driver->StopGimbal(impl_->core.identity().device_id); }

void DemoGimbal::Halt() {
  Clamp();
  impl_->core.LatchHalt();
}

DemoGimbalState DemoGimbal::ReadProprioception() const {
  const auto state =
      impl_->driver->ReadGimbal(impl_->core.identity().device_id).value_or(DemoGimbalState{});
  return DemoGimbalState{
      .pan_rad = state.pan_rad,
      .tilt_rad = state.tilt_rad,
      .pan_rate = state.pan_rate,
      .tilt_rate = state.tilt_rate,
  };
}

std::optional<hardware::GimbalTelemetry> DemoGimbal::ReadGimbalTelemetry() const {
  const DemoGimbalState state = ReadProprioception();
  return hardware::GimbalTelemetry{
      .pan_rad = state.pan_rad,
      .tilt_rad = state.tilt_rad,
      .pan_rate_rad_s = state.pan_rate,
      .tilt_rate_rad_s = state.tilt_rate,
  };
}

DemoGimbalHealth DemoGimbal::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoGimbalHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

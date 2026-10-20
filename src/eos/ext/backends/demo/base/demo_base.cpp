#include "demo_base.h"

#include <cmath>
#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"
#include "src/eos/lib/log.h"

namespace eos::backends::demo {

struct DemoBase::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;
  double footprint_width_m;
  double footprint_length_m;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world,
       double width_m, double length_m)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-base",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kBase},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 0.0},
                                .sensor = {.max_rate_hz = 0.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver),
        footprint_width_m(width_m),
        footprint_length_m(length_m) {}
};

DemoBase::DemoBase(std::string device_id, double footprint_width_m, double footprint_length_m)
    : impl_(
          new Impl(std::move(device_id),
                   std::make_shared<ext::simulator::ipc::SimulatorClient>("var/run/simulator.sock"),
                   footprint_width_m, footprint_length_m)) {}

DemoBase::DemoBase(std::string device_id,
                   std::shared_ptr<ext::simulator::ipc::SimulatorClient> world,
                   double footprint_width_m, double footprint_length_m)
    : impl_(new Impl(std::move(device_id), std::move(world), footprint_width_m,
                     footprint_length_m)) {}

DemoBase::~DemoBase() { delete impl_; }

hardware::BackendIdentity DemoBase::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoBase::DeclareFeeds() const { return impl_->core.feeds(); }

hardware::ReleaseResult DemoBase::ExecuteBase(const hardware::BaseCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if (!std::isfinite(cmd.linear_x_m_s) || !std::isfinite(cmd.linear_y_m_s) ||
      !std::isfinite(cmd.angular_z_rad_s) || std::abs(cmd.linear_x_m_s) > 1.0 ||
      std::abs(cmd.linear_y_m_s) > 1.0 || std::abs(cmd.angular_z_rad_s) > 1.0) {
    return hardware::ReleaseResult::kRejected;
  }
  return impl_->driver->DriveBase(impl_->core.identity().device_id, cmd.linear_x_m_s,
                                  cmd.linear_y_m_s, cmd.angular_z_rad_s)
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoBase::ExecuteNavigateRelative(
    const hardware::BaseNavigateRelativeCommand& command) {
  constexpr double kTwoPi = 6.28318530717958647692;
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if (!std::isfinite(command.x_m) || !std::isfinite(command.y_m) ||
      !std::isfinite(command.yaw_rad) || std::abs(command.x_m) > 10.0 ||
      std::abs(command.y_m) > 10.0 || std::abs(command.yaw_rad) > kTwoPi) {
    return hardware::ReleaseResult::kRejected;
  }
  return impl_->driver->NavigateBaseRelative(impl_->core.identity().device_id, command.x_m,
                                             command.y_m, command.yaw_rad, impl_->footprint_width_m,
                                             impl_->footprint_length_m)
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoBase::ExecutePosture(const hardware::BasePostureCommand& command) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if (command.posture != hardware::BasePosture::kStand &&
      command.posture != hardware::BasePosture::kWalk &&
      command.posture != hardware::BasePosture::kLie) {
    return hardware::ReleaseResult::kRejected;
  }
  return impl_->driver->SetBasePosture(impl_->core.identity().device_id,
                                       static_cast<std::uint8_t>(command.posture))
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoBase::StopMotion() {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  Clamp();
  return hardware::ReleaseResult::kAccepted;
}

void DemoBase::Clamp() { impl_->driver->StopBase(impl_->core.identity().device_id); }

void DemoBase::Halt() {
  Clamp();
  impl_->core.LatchHalt();
}

void DemoBase::Tick(double) {}

hardware::BasePosture DemoBase::ReadPosture() const {
  const auto state = impl_->driver->ReadBase(impl_->core.identity().device_id);
  return static_cast<hardware::BasePosture>(state ? state->posture : 0);
}

std::optional<hardware::BaseTelemetry> DemoBase::ReadBaseTelemetry() const {
  const DemoBaseOdometry state = ReadProprioception();
  return hardware::BaseTelemetry{
      .x_m = state.x,
      .y_m = state.y,
      .yaw_rad = state.yaw,
      .linear_x_m_s = state.vx,
      .linear_y_m_s = state.vy,
      .angular_z_rad_s = state.yaw_rate,
      .posture = ReadPosture(),
      .localization_valid = true,
  };
}

DemoBaseOdometry DemoBase::ReadProprioception() const {
  const auto state =
      impl_->driver->ReadBase(impl_->core.identity().device_id).value_or(DemoBaseOdometry{});
  return DemoBaseOdometry{
      .x = state.x,
      .y = state.y,
      .yaw = state.yaw,
      .vx = state.vx,
      .vy = state.vy,
      .yaw_rate = state.yaw_rate,
  };
}

DemoBaseHealth DemoBase::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoBaseHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

void DemoBase::LoadRasterMap(const std::filesystem::path&) {}

void DemoBase::LoadEmptyRasterMap() {
  // Map ownership belongs exclusively to eos-simulator.
}

}  // namespace eos::backends::demo

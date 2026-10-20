#include "demo_arm.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoArm::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;
  double link1_m;
  double link2_m;
  double mount_x_m;
  double mount_y_m;
  std::size_t joint_count;
  std::optional<std::string> last_pick_id;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world,
       double link1, double link2, double mount_x, double mount_y, std::size_t joints)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-arm",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kManipulator},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 10.0},
                                .sensor = {.max_rate_hz = 0.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver),
        link1_m(link1),
        link2_m(link2),
        mount_x_m(mount_x),
        mount_y_m(mount_y),
        joint_count(joints) {}
};

DemoArm::DemoArm(std::string device_id, double link1_m, double link2_m, double mount_x_m,
                 double mount_y_m, std::size_t joint_count)
    : DemoArm(std::move(device_id),
              std::make_shared<ext::simulator::ipc::SimulatorClient>("var/run/simulator.sock"),
              link1_m, link2_m, mount_x_m, mount_y_m, joint_count) {}

DemoArm::DemoArm(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world,
                 double link1_m, double link2_m, double mount_x_m, double mount_y_m,
                 std::size_t joint_count)
    : impl_(new Impl(std::move(device_id), std::move(world), link1_m, link2_m, mount_x_m, mount_y_m,
                     joint_count)) {}

DemoArm::~DemoArm() { delete impl_; }

hardware::BackendIdentity DemoArm::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoArm::DeclareFeeds() const { return impl_->core.feeds(); }

hardware::ReleaseResult DemoArm::ExecuteMoveL(const hardware::MoveLCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if ((cmd.speed_m_s && (!std::isfinite(*cmd.speed_m_s) || *cmd.speed_m_s <= 0.0)) ||
      !std::isfinite(cmd.position_m[0]) || !std::isfinite(cmd.position_m[1]) ||
      !std::isfinite(cmd.position_m[2])) {
    return hardware::ReleaseResult::kRejected;
  }
  const double quat_norm = cmd.orientation_quat_xyzw[0] * cmd.orientation_quat_xyzw[0] +
                           cmd.orientation_quat_xyzw[1] * cmd.orientation_quat_xyzw[1] +
                           cmd.orientation_quat_xyzw[2] * cmd.orientation_quat_xyzw[2] +
                           cmd.orientation_quat_xyzw[3] * cmd.orientation_quat_xyzw[3];
  if (!std::isfinite(quat_norm) || quat_norm < 0.999 || quat_norm > 1.001) {
    return hardware::ReleaseResult::kRejected;
  }
  // The demo models a planar two-link chain. Cartesian-to-joint kinematics
  // belong here at the backend edge; northbound dispatch only releases the
  // complete MoveL command.
  if (impl_->link1_m <= 0.0 || impl_->link2_m <= 0.0) {
    return hardware::ReleaseResult::kRejected;
  }
  const double x = cmd.position_m[0] - impl_->mount_x_m;
  const double y = cmd.position_m[1] - impl_->mount_y_m;
  const double reach = std::hypot(x, y);
  constexpr double kEpsilon = 1e-6;
  if (reach > impl_->link1_m + impl_->link2_m + kEpsilon ||
      reach < std::abs(impl_->link1_m - impl_->link2_m) - kEpsilon) {
    return hardware::ReleaseResult::kRejected;
  }
  const double cos_elbow =
      (reach * reach - impl_->link1_m * impl_->link1_m - impl_->link2_m * impl_->link2_m) /
      (2.0 * impl_->link1_m * impl_->link2_m);
  const double elbow = std::acos(std::clamp(cos_elbow, -1.0, 1.0));
  const double shoulder =
      std::atan2(y, x) - std::atan2(impl_->link2_m * std::sin(elbow),
                                    impl_->link1_m + impl_->link2_m * std::cos(elbow));
  (void)shoulder;
  (void)elbow;
  return impl_->driver->SetCartesian(impl_->core.identity().device_id, cmd.position_m,
                                     cmd.orientation_quat_xyzw)
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoArm::ExecuteMoveJ(const hardware::MoveJCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  if (cmd.joint_positions_rad.size() != impl_->joint_count ||
      (cmd.speed_rad_s && (!std::isfinite(*cmd.speed_rad_s) || *cmd.speed_rad_s <= 0.0))) {
    return hardware::ReleaseResult::kRejected;
  }
  constexpr double kJointLimit = 6.28;
  for (const double position_rad : cmd.joint_positions_rad) {
    if (!std::isfinite(position_rad) || position_rad < -kJointLimit || position_rad > kJointLimit) {
      return hardware::ReleaseResult::kRejected;
    }
  }
  return impl_->driver->SetJointPositions(impl_->core.identity().device_id, cmd.joint_positions_rad)
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

hardware::ReleaseResult DemoArm::ExecutePick(const hardware::PickCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted || cmd.object_id.empty()) {
    return hardware::ReleaseResult::kRejected;
  }
  // Picks are deliberately independent in the simple demo: accepting one
  // does not move the arm, close a gripper, or mutate perception state.
  impl_->last_pick_id = cmd.object_id;
  return hardware::ReleaseResult::kAccepted;
}

hardware::ReleaseResult DemoArm::StopMotion() {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  Clamp();
  return hardware::ReleaseResult::kAccepted;
}

void DemoArm::Clamp() { impl_->driver->StopArm(impl_->core.identity().device_id); }

void DemoArm::Halt() {
  Clamp();
  impl_->core.LatchHalt();
}

DemoArmProprioception DemoArm::ReadProprioception() const {
  return impl_->driver->ReadArm(impl_->core.identity().device_id).value_or(DemoArmProprioception{});
}

std::optional<hardware::ManipulatorTelemetry> DemoArm::ReadManipulatorTelemetry() const {
  const DemoArmProprioception state = ReadProprioception();
  hardware::ManipulatorTelemetry result;
  for (std::size_t index = 0; index < result.joint_angles_rad.size(); ++index) {
    result.joint_angles_rad[index] = state.joint_positions_rad[index];
    result.joint_velocities_rad_s[index] = state.joint_velocities[index];
    result.joint_valid[index] = index < impl_->joint_count;
  }
  std::copy(std::begin(state.ee_position), std::end(state.ee_position), result.position_m);
  std::copy(std::begin(state.ee_orientation_quat), std::end(state.ee_orientation_quat),
            result.orientation_quat);
  result.pose_valid = impl_->driver->Available(impl_->core.identity().device_id);
  return result;
}

DemoArmHealth DemoArm::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoArmHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

/// Demo robotic arm backend — loopback manipulator stub. Implements the
/// south-bound interface declared in eos/hardware/backend.h with an
/// in-process kinematic point integrator standing in for the arm; no
/// hardware, no middleware, no physics. Same release semantics as the sim
/// backend, plus a stub joint-state proprioception feed.
#pragma once

#include <memory>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Stub arm proprioception: EE state plus fixed-size joint positions.
using DemoArmProprioception = ext::simulator::ipc::ArmState;
using DemoArmHealth = hardware::BackendHealth;

/// Thin adapter from EOS manipulator commands to the external demo driver.
/// The driver owns all mock state and behavior.
class DemoArm final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoArm(std::string device_id, double link1_m = 0.7, double link2_m = 0.5,
                   double mount_x_m = 0.0, double mount_y_m = 0.0, std::size_t joint_count = 6);
  DemoArm(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world,
          double link1_m = 0.7, double link2_m = 0.5, double mount_x_m = 0.0,
          double mount_y_m = 0.0, std::size_t joint_count = 6);
  ~DemoArm() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteMoveJ(const hardware::MoveJCommand& cmd) override;
  hardware::ReleaseResult ExecuteMoveL(const hardware::MoveLCommand& cmd) override;
  hardware::ReleaseResult ExecutePick(const hardware::PickCommand& cmd) override;
  hardware::ReleaseResult StopMotion() override;
  std::optional<hardware::ManipulatorTelemetry> ReadManipulatorTelemetry() const override;
  void Clamp() override;
  void Halt() override;

  /// Stub feed reads for structure tests.
  DemoArmProprioception ReadProprioception() const;
  DemoArmHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

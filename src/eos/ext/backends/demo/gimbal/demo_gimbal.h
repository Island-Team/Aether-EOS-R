/// Demo gimbal backend — loopback pan/tilt stub. Implements the south-bound interface declared in
/// eos/hardware/backend.h: accepts radian GimbalCommand releases, stores
/// the pan/tilt target as device state, and exposes a stub pan/tilt
/// proprioception feed. Perception and scene generation remain inside EOS and
/// never cross this hardware adapter. No hardware or middleware is required.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Stub gimbal state: current pan/tilt angles and rates, radians and rad/s.
using DemoGimbalState = ext::simulator::ipc::GimbalState;
using DemoGimbalHealth = hardware::BackendHealth;

/// Thin adapter from EOS gimbal commands to the external simulator driver.
/// The driver owns mock pan/tilt state.
class DemoGimbal final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoGimbal(std::string device_id);
  DemoGimbal(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world);
  ~DemoGimbal() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteGimbal(const hardware::GimbalCommand& cmd) override;
  hardware::ReleaseResult StopMotion() override;
  std::optional<hardware::GimbalTelemetry> ReadGimbalTelemetry() const override;
  void Clamp() override;
  void Halt() override;

  /// Stub feed reads for structure tests.
  DemoGimbalState ReadProprioception() const;
  DemoGimbalHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

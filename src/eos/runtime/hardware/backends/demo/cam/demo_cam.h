/// Demo camera backend — feed-only projection of the shared world's selected
/// and decoded visual source into the south-bound camera contract.
/// Rejects every release: a camera has no actuation path. Used for structure
/// and dataflow tests of the sensor feed path.
#pragma once

#include <memory>
#include <string>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

using DemoCamHealth = hardware::BackendHealth;

/// In-process world camera: reads world-owned frames and accepts no commands.
class DemoCam final : public hardware::Backend {
 public:
  struct FrameContext {
    ext::simulator::ipc::StateSnapshot state;
  };

  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoCam(std::string device_id);
  DemoCam(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world);
  ~DemoCam() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  /// Returns the source selected and decoded by the shared demo world.
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override;
  void Tick(double dt_s) override;
  void Clamp() override;
  void Halt() override;

  DemoCamHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

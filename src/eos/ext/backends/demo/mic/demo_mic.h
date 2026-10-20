/// Demo microphone backend — feed-only loopback device. Implements the
/// south-bound interface declared in eos/hardware/backend.h with a
/// fixed stub PCM block; no hardware, no middleware, no real audio. Rejects
/// every release: a microphone has no actuation path. Used for structure and
/// dataflow tests of the sensor feed path.
#pragma once

#include <memory>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Stub PCM block the demo microphone produces. Fixed ramp samples; the real
/// feed path publishes blocks within the declared rate limit.
using DemoMicBlock = ext::simulator::ipc::MicrophoneBlock;
using DemoMicHealth = hardware::BackendHealth;

/// In-process stub microphone: publishes fixed PCM blocks, accepts no
/// commands.
class DemoMic final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoMic(std::string device_id);
  DemoMic(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world);
  ~DemoMic() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  void Clamp() override;
  void Halt() override;

  /// Stub feed reads for structure tests.
  DemoMicBlock ReadSensor() const;

  /// The backend-neutral PCM window the runtime reads. The runtime never sees
  /// `DemoMicBlock`; this override is the seam that keeps it decoupled from the
  /// demo world type.
  std::optional<hardware::MicrophoneTelemetry> ReadMicrophoneTelemetry() const override;

  DemoMicHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

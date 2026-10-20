/// Demo speaker backend — loopback audio stub. Implements the south-bound
/// interface declared in eos/hardware/backend.h: accepts
/// SpeakerCommand releases, tracks playback as a countdown, and exposes a
/// stub speaker proprioception feed. No hardware, no middleware, no audio.
#pragma once

#include <memory>
#include <string>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

using DemoSpeakerHealth = hardware::BackendHealth;

/// In-process stub speaker that accepts one complete audio asset per release.
/// Tick advances playback;
/// Clamp stops playback; Halt also latches the safe envelope.
class DemoSpeaker final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoSpeaker(std::string device_id);
  DemoSpeaker(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world);
  ~DemoSpeaker() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteSpeaker(const hardware::SpeakerCommand& cmd) override;
  void Tick(double dt_s) override;
  void Clamp() override;
  void Halt() override;

  /// Returns the loopback driver's semantic playback state.
  std::optional<hardware::SpeakerTelemetry> ReadSpeakerTelemetry() const override;
  DemoSpeakerHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

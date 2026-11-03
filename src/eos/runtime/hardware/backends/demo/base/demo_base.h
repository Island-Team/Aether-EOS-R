/// Demo mobile base backend — loopback planar-drive stub. Implements the
/// south-bound interface declared in eos/hardware/backend.h: accepts
/// SI-unit BaseCommand releases, integrates planar pose from the stored
/// twist, and
/// exposes a stub odometry proprioception feed. No hardware, no middleware.
#pragma once

#include <filesystem>
#include <memory>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Stub base odometry: integrated planar pose plus last commanded twist.
using DemoBaseOdometry = ext::simulator::ipc::BaseState;
using DemoBaseHealth = hardware::BackendHealth;

/// Thin adapter from EOS base commands to the external demo driver. The
/// driver owns mock odometry and integration.
class DemoBase final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoBase(std::string device_id, double footprint_width_m = 0.3,
                    double footprint_length_m = 0.5);
  DemoBase(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world,
           double footprint_width_m = 0.3, double footprint_length_m = 0.5);
  ~DemoBase() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteBase(const hardware::BaseCommand& cmd) override;
  hardware::ReleaseResult ExecuteNavigateRelative(
      const hardware::BaseNavigateRelativeCommand& command) override;
  hardware::ReleaseResult ExecutePosture(const hardware::BasePostureCommand& command) override;
  hardware::ReleaseResult StopMotion() override;
  std::optional<hardware::BaseTelemetry> ReadBaseTelemetry() const override;
  void Clamp() override;
  void Halt() override;

  /// Advances the stub integrator by `dt` seconds under the stored twist.
  /// Structure tests call this explicitly; the real feed path ticks at the
  /// declared rate limit.
  void Tick(double dt);

  /// Current discrete posture; kStand until an ExecutePosture release
  /// changes it. Kept out of the odometry feed so the feed envelope stays
  /// the twist-and-pose struct alone.
  hardware::BasePosture ReadPosture() const;

  /// Stub feed reads for structure tests.
  DemoBaseOdometry ReadProprioception() const;
  DemoBaseHealth ReadHealth() const override;

  /// Loads the configured persisted raster bundle and applies its initial pose.
  void LoadRasterMap(const std::filesystem::path& bundle_path);

  /// Uses a finite all-free raster when no persisted simulator map is available.
  void LoadEmptyRasterMap();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

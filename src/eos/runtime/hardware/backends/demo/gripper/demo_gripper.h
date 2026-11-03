/// Demo gripper backend — loopback jaw stub. Implements the south-bound
/// interface declared in eos/hardware/backend.h: accepts
/// GripperCommand releases and tracks the semantic jaw state (open, closed
/// on nothing, holding a grasped object). The wire command carries no object
/// id — the dispatch layer arms the grasp target demo-side via ArmGrasp()
/// before releasing the close, so the pick flow stays an orchestration
/// concern. No hardware, no middleware, no real grasp force.
#pragma once

#include <memory>
#include <string>

#include "eos/hardware/backend.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

/// Semantic jaw state. Values spell the wire literals the projection layer
/// serves; `kHolding` is the only state with a held object id.
using DemoGripperJaw = ext::simulator::ipc::GripperJaw;

/// Stub gripper proprioception. Field names mirror the contract's gripper
/// state one-to-one so the projection maps them without renaming.
using DemoGripperState = ext::simulator::ipc::GripperState;
using DemoGripperHealth = hardware::BackendHealth;

/// In-process stub gripper: ExecuteGripper realizes open/close intent — a
/// close consumes an armed grasp target (if any) into `kHolding`, an open
/// releases any held object. Clamp stops applying force while holding the
/// jaw state; Halt latches the safe envelope and refuses further release.
class DemoGripper final : public hardware::Backend {
 public:
  /// Constructs the adapter from the `demo_{kind}` profile of a resolved etc/demo.toml
  /// binding. `device_id` names the simulated device handle.
  explicit DemoGripper(std::string device_id);
  DemoGripper(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world);
  ~DemoGripper() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteGripper(const hardware::GripperCommand& cmd) override;
  /// Projects simulated jaw state and force into the backend-neutral feed.
  std::optional<hardware::GripperTelemetry> ReadGripperTelemetry() const override;
  void Clamp() override;
  void Halt() override;

  /// Arms `object_id` as the grasp target the next close consumes. Demo-level
  /// orchestration hook, not a release: the wire GripperCommand carries no
  /// object id, so the dispatch layer (which orchestrates pick across
  /// arm + vision + gripper) stages the target here before releasing the
  /// close. One-shot — consumed by the next close; survives intermediate
  /// commands; an empty id disarms.
  void ArmGrasp(const std::string& object_id);

  /// Stub feed reads for structure tests and the composition layers.
  DemoGripperState ReadProprioception() const;
  DemoGripperHealth ReadHealth() const override;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::backends::demo

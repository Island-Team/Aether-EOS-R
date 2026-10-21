/// ROS 2 device bridge over the standard common interfaces only. The
/// only module with an rclcpp dependency; vendor drivers, vendor message
/// types, and the graph-edge adapter that converts them stay outside EOS.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"
#include "src/eos/ext/backends/ros2/config.h"

namespace eos::backends::ros2 {

/// Owns a side node in the user's existing ROS graph. Every endpoint is a
/// topic of a standard message type named by the binding's
/// [device.backend.ros2] endpoint groups; a deployment whose hardware speaks
/// a vendor protocol runs an adapter that bridges the two graph sides.
class Ros2Backend final : public hardware::Backend {
 public:
  /// `joint_names` carries the ordered URDF joint names from the device's
  /// `device.joint_names`; empty falls back to joint_1..joint_6.
  /// Owns its context and executor thread; throws on invalid configuration.
  Ros2Backend(std::string device_id, config::Ros2DeviceConfig config,
              hardware::DeviceKind kind, std::vector<std::string> joint_names = {});
  /// Stops and joins the executor before releasing middleware resources.
  ~Ros2Backend() override;

  /// Returns the admitted device kind and immutable transport identity.
  hardware::BackendIdentity Identify() const override;
  /// Declares the projection rate envelope for this device class.
  hardware::FeedDeclarations DeclareFeeds() const override;
  /// Publishes ordered radians; accepted means local publication, not completion.
  hardware::ReleaseResult ExecuteMoveJ(const hardware::MoveJCommand& cmd) override;
  /// Publishes metres and XYZW; rejects invalid or unhealthy commands.
  hardware::ReleaseResult ExecuteMoveL(const hardware::MoveLCommand& cmd) override;
  /// Publishes one typed world-object ID; accepted means local publication.
  hardware::ReleaseResult ExecutePick(const hardware::PickCommand& cmd) override;
  /// Preserves fresh measured arm joints except for configured pan and tilt joints.
  hardware::ReleaseResult ExecuteGimbal(const hardware::GimbalCommand& cmd) override;
  /// Publishes open/close and optional newtons using the standard array protocol.
  hardware::ReleaseResult ExecuteGripper(const hardware::GripperCommand& cmd) override;
  /// Copies arm state under a mutex, clearing validity for each expired stream.
  std::optional<hardware::ManipulatorTelemetry> ReadManipulatorTelemetry() const override;
  std::optional<hardware::GimbalTelemetry> ReadGimbalTelemetry() const override;
  /// Copies a fresh gripper sample; unavailable measurements remain optional.
  std::optional<hardware::GripperTelemetry> ReadGripperTelemetry() const override;
  /// Copies bounded JPEG bytes; absent or expired samples return nullopt.
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override;
  /// Publishes an ordinary motion stop only when the driver is ready to receive it.
  hardware::ReleaseResult StopMotion() override;
  hardware::ReleaseResult CancelCommand(const std::string& command_id) override;
  std::string ReleaseFailureReason() const override;
  /// Best-effort safety clamp, including non-motion devices during estop.
  void Clamp() override;
  /// Latches local rejection and publishes the configured estop endpoint.
  void Halt() override;

  /// Reads telemetry freshness and optional adapter health under a mutex.
  hardware::BackendHealth ReadHealth() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::ros2

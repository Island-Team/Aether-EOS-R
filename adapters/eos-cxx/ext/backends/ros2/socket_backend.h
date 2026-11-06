#pragma once

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "eos/hardware/backend.h"

namespace eos::backends::ros2 {
// Backend-neutral proxy. EOS never loads ROS middleware or owns a driver.
class SocketBackend final : public hardware::Backend {
 public:
  SocketBackend(std::string device_id, hardware::DeviceKind kind, std::string socket_path);
  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::BackendHealth ReadHealth() const override;
  hardware::ReleaseResult ExecuteMoveJ(const hardware::MoveJCommand&) override;
  hardware::ReleaseResult ExecuteMoveL(const hardware::MoveLCommand&) override;
  hardware::ReleaseResult ExecutePick(const hardware::PickCommand&) override;
  hardware::ReleaseResult ExecuteGimbal(const hardware::GimbalCommand&) override;
  hardware::ReleaseResult ExecuteGripper(const hardware::GripperCommand&) override;
  hardware::ReleaseResult StopMotion() override;
  hardware::ReleaseResult CancelCommand(const std::string&) override;
  std::optional<hardware::ManipulatorTelemetry> ReadManipulatorTelemetry() const override;
  std::optional<hardware::GimbalTelemetry> ReadGimbalTelemetry() const override;
  std::optional<hardware::GripperTelemetry> ReadGripperTelemetry() const override;
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override;
  std::string ReleaseFailureReason() const override;
  void Clamp() override;
  void Halt() override;
 private:
  using Json = nlohmann::json;
  Json Request(const std::string& operation, Json arguments = Json::object()) const;
  hardware::ReleaseResult Release(const std::string& operation, Json arguments = Json::object());
  std::string device_id_;
  hardware::DeviceKind kind_;
  std::string socket_path_;
  mutable std::mutex error_mutex_;
  mutable std::string last_error_{"ROS bridge is unavailable"};
};
}  // namespace eos::backends::ros2

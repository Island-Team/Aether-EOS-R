/// Owns admitted south-bound backends; has no north-bound contract dependency.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binding.h"
#include "eos/hardware/backend.h"

namespace eos::backends::demo {
class DemoArm;
class DemoBase;
class DemoGimbal;
class DemoGripper;
class DemoSpeaker;
}  // namespace eos::backends::demo

namespace eos::ext::simulator::ipc {
class SimulatorClient;
}

namespace eos::runtime::dispatch {
class Dispatcher;
}

namespace eos::runtime::hardware_runtime {

/// `alive` clears after Halt; `admitted` records startup admission.
using DeviceHealth = hardware::BackendHealth;

/// Owns one backend and its admitted configuration.
class Device {
 public:
  /// Owns `backend` and its admitted configuration.
  Device(std::unique_ptr<hardware::Backend> backend, std::string robot_id, PartConfig part,
         hardware::FeedDeclarations envelope);

  /// Command methods are private to Registry and Dispatcher.
  hardware::BackendIdentity identity() const { return backend_->Identify(); }
  const hardware::Backend& feeds() const { return *backend_; }

  /// Runtime robot this device is mounted on.
  const std::string& robot_id() const { return robot_id_; }

  const PartConfig& part() const { return part_; }
  const hardware::FeedDeclarations& envelope() const { return envelope_; }

  /// Per-device health: alive until halted, admitted by construction.
  DeviceHealth ReadHealth() const { return backend_->ReadHealth(); }

 private:
  hardware::Backend& backend() { return *backend_; }
  const hardware::Backend& backend() const { return *backend_; }
  friend class Registry;
  friend class eos::runtime::dispatch::Dispatcher;
  std::unique_ptr<hardware::Backend> backend_;
  std::string robot_id_;
  PartConfig part_;
  hardware::FeedDeclarations envelope_;
};

/// Startup admission barrier for one process-scoped hardware binding.
class Registry {
 public:
  Registry(Registry&&) noexcept;
  Registry& operator=(Registry&&) noexcept;

  /// Loads a binding file and admits it; the module's entry point. A
  /// non-empty `profile` selects one `[profile.<name>]` table of a profiled
  /// file (etc/eos/backends/demo.toml). Throws std::runtime_error naming the file and
  /// the problem on any parse, factory, or admission failure.
  static Registry Load(const std::string& binding_path, const std::string& profile = "",
                       const std::string& runtime_id = "");

  /// Loads and admits exactly one `(binding path, profile)` entry.
  static Registry Load(const std::vector<std::pair<std::string, std::string>>& bindings,
                       const std::string& runtime_id = "");

  /// Admits an already-parsed binding. Throws std::runtime_error naming the
  /// binding path, device, and problem on factory or admission failure.
  static Registry Adopt(Binding binding, const std::string& runtime_id = "",
                        const std::filesystem::path& simulator_socket = "var/run/simulator.sock");

  /// Admits exactly one parsed hardware binding.
  static Registry Adopt(std::vector<Binding> bindings, const std::string& runtime_id = "",
                        const std::filesystem::path& simulator_socket = "var/run/simulator.sock");

  /// Admitted devices in binding order.
  const std::vector<Device>& devices() const { return devices_; }

  /// Process-scoped robot identity plus binding hardware metadata.
  const RobotIdentity& robot() const { return robot_; }

  /// Compatibility view containing the process's single robot identity.
  const std::vector<RobotIdentity>& robots() const { return robots_; }

  /// Robot identity by runtime UUID, or nullptr when absent.
  const RobotIdentity* FindRobot(const std::string& robot_id) const;

  /// Device by process-unique generated identity, or nullptr when absent.
  const Device* FindDevice(const std::string& device_id) const;

  /// First device of `kind`. Throws std::runtime_error naming the kind when
  /// the binding declared none; use the two-argument form when several
  /// devices share a kind.
  const Device& Find(hardware::DeviceKind kind) const;

  /// Device of `kind` carrying `part_id`. Throws std::runtime_error naming
  /// kind and part when absent.
  const Device& Find(hardware::DeviceKind kind, const std::string& part_id) const;

  /// Demo-scale composition accessors: the concrete demo behind the first
  /// device of the kind with the given part (default "default"), or nullptr
  /// when absent — including when the device was built by a non-demo backend
  /// (the cast is dynamic off the polymorphic Backend base, never a guess).
  /// For the composition layers and tests only — not part of the Backend
  /// contract.
  const backends::demo::DemoBase* Base(const std::string& part_id = "default") const;
  const backends::demo::DemoArm* Arm(const std::string& part_id = "default") const;
  const backends::demo::DemoGimbal* Gimbal(const std::string& part_id = "default") const;
  const backends::demo::DemoGripper* Gripper(const std::string& part_id = "default") const;
  const backends::demo::DemoSpeaker* Speaker(const std::string& part_id = "default") const;

  /// Advances backend-local integrators. External simulators own their clocks,
  /// so demo adapters intentionally no-op here.
  void Tick(double dt_s);

  /// True only after the explicit process-wide safety.estop primitive.
  bool EstopLatched() const { return estop_latched_.load(); }

 private:
  Registry() = default;
  Device* FindDeviceForRelease(const std::string& device_id);
  void LatchEstop();
  friend class eos::runtime::dispatch::Dispatcher;

  std::vector<Device> devices_;
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_client_;
  RobotIdentity robot_{};
  std::vector<RobotIdentity> robots_;
  std::atomic_bool estop_latched_{false};
};

}  // namespace eos::runtime::hardware_runtime

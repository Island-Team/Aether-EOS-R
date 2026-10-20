/// Backend-aligned primitive dispatch. Every accepted call resolves one
/// admitted device and performs one south-bound release. Retained state covers
/// command idempotency, bounded per-device admission, and safety leases.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/init/schemas.h"

namespace eos::runtime::models {
class Runtime;
}
namespace eos::runtime::hardware_runtime {
class TelemetryHub;
}

namespace eos::runtime::dispatch {

struct DispatchResult {
  enum class Status {
    kCompleted,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kFailedPrecondition,
    kUnavailable,
    kInternal
  };
  Status status{Status::kInternal};
  nlohmann::json payload;
  std::string message;
};

enum class NavigationCancelStatus { kCancelled, kNotFound, kMismatch, kUncertain };

/// Bounds FIFO terminal-command admission for each physical device.
struct AdmissionOptions {
  /// Maximum admitted-or-waiting commands retained for one device.
  std::size_t max_commands_per_device{64};
  /// Maximum time a command may wait behind earlier commands.
  std::chrono::milliseconds max_wait{std::chrono::seconds(30)};
};

class Dispatcher {
 public:
  Dispatcher(hardware_runtime::Registry& registry, const init::SchemaStore& schemas,
             bool expose_operator = false, models::Runtime* models = nullptr,
             std::filesystem::path assets_root = {}, std::filesystem::path command_journal = {},
             AdmissionOptions admission = {});
  ~Dispatcher();
  DispatchResult Execute(const std::string& primitive, const std::string& version,
                         const std::string& device_id, nlohmann::json arguments);
  /// Executes one globally identified command at most once. Identical retries
  /// join/replay the first result; conflicting reuse never reaches a backend.
  DispatchResult ExecuteCommand(const std::string& primitive, const std::string& version,
                                const std::string& device_id, nlohmann::json arguments,
                                const std::string& command_id,
                                const std::function<bool()>& cancelled = {});
  /// Requests cancellation of one active cancellable command and returns its
  /// terminal response. Repeated cancellation never emits a second stop.
  DispatchResult CancelCommand(const std::string& command_id);
  /// Cancels only an active base.navigate command on the expected base device.
  NavigationCancelStatus CancelActiveNavigationCommand(const std::string& command_id,
                                                        const std::string& device_id);
  /// Executes one skill step and returns only after its terminal boundary.
  /// Primitives without a completion strategy fail closed for skill callers.
  DispatchResult ExecuteSkillStep(const std::string& primitive, const std::string& version,
                                  const std::string& device_id, nlohmann::json arguments,
                                  const std::function<bool()>& cancelled = {});
  nlohmann::json Discover() const;
  std::vector<init::PrimitiveIdentity> PrimitiveIdentities() const;
  void TickHardware(double dt_s);
  void Tick(double dt_s);
  void SetTelemetryHub(hardware_runtime::TelemetryHub* telemetry) { telemetry_ = telemetry; }

 private:
  nlohmann::json Ack(const std::string&, const std::string&, const nlohmann::json&, bool,
                     const char*, const std::string&);
  nlohmann::json Accepted(const std::string&, const std::string&, const nlohmann::json&);
  nlohmann::json Released(const std::string&, const std::string&, const nlohmann::json&,
                          hardware_runtime::Device&, hardware::ReleaseResult, const std::string&);
  hardware_runtime::Device* Target(const std::string&, const std::string&, const std::string&,
                                   hardware::DeviceKind, const nlohmann::json&, nlohmann::json*);
  nlohmann::json BaseVelocity(const std::string&, const std::string&, hardware_runtime::Device&,
                              const nlohmann::json&);
  nlohmann::json BaseNavigate(const std::string&, const std::string&, hardware_runtime::Device&,
                              const nlohmann::json&);
  nlohmann::json BasePosture(const std::string&, const std::string&, hardware_runtime::Device&,
                             const nlohmann::json&);
  nlohmann::json GimbalMotion(const std::string&, const std::string&, hardware_runtime::Device&,
                              const nlohmann::json&);
  nlohmann::json ArmInit(const std::string&, const std::string&, hardware_runtime::Device&,
                         const nlohmann::json&);
  nlohmann::json ArmMoveJ(const std::string&, const std::string&, hardware_runtime::Device&,
                          const nlohmann::json&);
  nlohmann::json ArmMoveL(const std::string&, const std::string&, hardware_runtime::Device&,
                          const nlohmann::json&);
  nlohmann::json ArmPick(const std::string&, const std::string&, hardware_runtime::Device&,
                         const nlohmann::json&);
  nlohmann::json GripperMotion(const std::string&, const std::string&, hardware_runtime::Device&,
                               const nlohmann::json&);
  nlohmann::json SpeakerPlay(const std::string&, const std::string&, hardware_runtime::Device&,
                             const nlohmann::json&);
  nlohmann::json SpeakerSpeak(const std::string&, const std::string&, hardware_runtime::Device&,
                              const nlohmann::json&);
  nlohmann::json FaceRegister(const std::string&, const std::string&, hardware_runtime::Device&,
                              const nlohmann::json&);
  nlohmann::json ReleaseSpeaker(const std::string&, const std::string&, hardware_runtime::Device&,
                                const nlohmann::json&, hardware::SpeakerCommand);
  nlohmann::json MotionStop(const std::string&, const std::string&, hardware_runtime::Device&,
                            const nlohmann::json&);
  nlohmann::json RobotStop(const std::string&, const std::string&, const nlohmann::json&);
  nlohmann::json Estop(const std::string&, const std::string&, const nlohmann::json&);
  void HaltDevice(hardware_runtime::Device& device);
  static bool DeclaresPrimitive(const hardware_runtime::Device&, const std::string&,
                                const std::string&);
  nlohmann::json ExecuteAdmitted(const std::string&, const std::string&, const std::string&,
                                 const nlohmann::json&);
  DispatchResult ExecuteTerminal(const std::string&, const std::string&, const std::string&,
                                 nlohmann::json, const std::function<bool()>&,
                                 const std::function<void()>&);
  std::mutex* DeviceMutex(const std::string& device_id) const;
  static bool IsCancellable(const std::string& primitive);

  struct CommandRecord {
    std::string fingerprint;
    std::string primitive;
    std::string version;
    std::string device_id;
    bool cancellable{false};
    std::atomic<bool> cancel_requested{false};
    bool cancel_released{false};
    bool backend_started{false};
    bool complete{false};
    DispatchResult result;
    std::condition_variable changed;
  };
  void LoadCommandJournal();
  void AppendCommandJournal(const nlohmann::json& entry) const;

  hardware_runtime::Registry& registry_;
  const init::SchemaStore& schemas_;
  bool expose_operator_{false};
  models::Runtime* models_{nullptr};
  hardware_runtime::TelemetryHub* telemetry_{nullptr};
  std::filesystem::path assets_root_;
  std::filesystem::path command_journal_;
  AdmissionOptions admission_;
  mutable std::mutex command_mutex_;
  std::condition_variable admission_changed_;
  std::map<std::string, std::shared_ptr<CommandRecord>> commands_;
  std::map<std::string, std::deque<std::string>> device_admission_;
  mutable std::mutex state_mutex_;
  std::map<std::string, std::unique_ptr<std::mutex>> device_mutexes_;
  std::atomic<std::uint64_t> ack_sequence_{0};
  std::map<std::string, double> base_velocity_lease_s_;
  struct GimbalStreamTarget {
    double pan_rad{0.0};
    double tilt_rad{0.0};
  };
  std::map<std::string, GimbalStreamTarget> gimbal_stream_targets_;
};

}  // namespace eos::runtime::dispatch

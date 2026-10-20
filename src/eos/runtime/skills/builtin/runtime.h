#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/eos/runtime/skills/builtin/result.h"
#include "src/eos/runtime/skills/builtin/task.h"
#include "src/eos/runtime/skills/core/provider.h"

namespace eos::runtime::skills::builtin {

struct PrimitiveOperation {
  std::string operation_id;
  std::string command_id;
};
enum class NavigationCancelStatus { kCancelled, kNotFound, kMismatch, kUncertain };
struct InferenceOperation { std::string operation_id; };

struct ResourcePublication {
  std::uint64_t sequence{0};
  std::chrono::system_clock::time_point observed_at;
  std::string boot_id;
  std::string epoch;
  nlohmann::json value;
};

struct ResourceRetention {
  std::uint64_t first_available_sequence{0};
  std::uint64_t latest_sequence{0};
  std::uint64_t evicted_through_sequence{0};
  bool closed{false};
};

enum class ResourceSelectorKind {
  kLatest,
  kExactSequence,
  kExactTime,
  kBefore,
  kAfter,
  kNearest,
  kSequenceRange,
  kTimeRange,
};

struct ResourceSelector {
  ResourceSelectorKind kind{ResourceSelectorKind::kLatest};
  std::uint64_t first_sequence{0};
  std::uint64_t last_sequence{0};
  std::chrono::system_clock::time_point first_time;
  std::chrono::system_clock::time_point last_time;
  std::size_t limit{1};
};

struct ResourceHistory {
  std::vector<ResourcePublication> publications;
  ResourceRetention retention;
  bool retention_gap{false};
  bool truncated{false};
};

enum class ResourceWaitStatus { kPublication, kTimeout, kCancelled, kClosed };

struct ResourceWait {
  ResourceWaitStatus status{ResourceWaitStatus::kClosed};
  std::optional<ResourcePublication> publication;
  ResourceRetention retention;
  bool retention_gap{false};
};

namespace detail {
struct ResourceSubscriptionState;
}

class ResourceSubscription final {
 public:
  ResourceSubscription() = default;
  ResourceSubscription(ResourceSubscription&&) noexcept = default;
  ResourceSubscription& operator=(ResourceSubscription&&) noexcept = default;
  ResourceSubscription(const ResourceSubscription&) = delete;
  ResourceSubscription& operator=(const ResourceSubscription&) = delete;
  explicit operator bool() const noexcept { return state_ != nullptr; }

 private:
  friend class Registry;
  friend class Execution;
  explicit ResourceSubscription(std::shared_ptr<detail::ResourceSubscriptionState> state)
      : state_(std::move(state)) {}
  std::shared_ptr<detail::ResourceSubscriptionState> state_;
};

struct CapabilityHooks {
  std::function<Result<PrimitiveOperation>(const PrimitiveReference&, const std::string&,
                                            nlohmann::json)> start_primitive;
  std::function<Result<std::optional<nlohmann::json>>(const PrimitiveOperation&,
                                                       std::chrono::milliseconds)> wait_primitive;
  std::function<Result<void>(const PrimitiveOperation&)> cancel_primitive;
  std::function<Result<NavigationCancelStatus>(const std::string&, const std::string&)>
      cancel_navigation_command;
  std::function<Result<nlohmann::json>(const std::string&)> read_resource;
  std::function<Result<ResourceSubscription>(const std::string&)> open_resource_subscription;
  std::function<Result<ResourceHistory>(const ResourceSubscription&, const ResourceSelector&)>
      read_resource_history;
  std::function<Result<ResourceWait>(const ResourceSubscription&, std::uint64_t,
                                     std::chrono::milliseconds)> wait_resource;
  std::function<Result<void>(ResourceSubscription&)> close_resource_subscription;
  std::function<Result<void>(nlohmann::json)> log_detection_attachment;
  std::function<Result<void>(nlohmann::json)> set_detection_overlay;
  std::function<Result<void>()> clear_detection_overlay;
  std::function<Result<std::string>()> default_object_camera;
  std::function<Result<nlohmann::json>(const std::string&)> observe_objects;
  std::function<Result<nlohmann::json>(nlohmann::json)> plan_grasp;
  std::function<Result<nlohmann::json>()> grasp_observe_joints;
  std::function<Result<std::string>()> grasp_wrist_camera;
  std::function<Result<nlohmann::json>()> grasp_workspace_bounds;
  std::function<Result<nlohmann::json>(const std::string&, std::chrono::steady_clock::time_point)> observe_objects_fresh;
  std::function<Result<InferenceOperation>(InferenceRequest)> start_inference;
  std::function<Result<std::optional<InferenceResult>>(const InferenceOperation&,
                                                        std::chrono::milliseconds)> wait_inference;
  std::function<Result<void>(const InferenceOperation&)> cancel_inference;
};

struct Outcome {
  RunResult::Status status{RunResult::Status::kFailed};
  bool accepted{false};
  std::string reason_code;
  nlohmann::json result = nlohmann::json::object();
  std::string message;

  static Outcome Completed(nlohmann::json result = nlohmann::json::object());
  static Outcome Rejected(std::string reason_code, std::string message = {});
  static Outcome InvalidArgument(std::string reason_code, std::string message = {});
  static Outcome Cancelled(std::string reason_code = "cancelled", std::string message = {});
  static Outcome Failed(std::string reason_code, std::string message = {});
};

class Execution {
 public:
  Execution(eos::lib::TimerService& timers, const nlohmann::json& arguments,
            std::string command_id, ExecutionHooks hooks, CapabilityHooks capabilities = {})
      : Execution(timers, arguments, {}, std::move(command_id), std::move(hooks),
                  std::move(capabilities)) {}
  Execution(eos::lib::TimerService& timers, const nlohmann::json& arguments,
             eos::lib::CancellationToken provider_cancellation, std::string command_id,
             ExecutionHooks hooks, CapabilityHooks capabilities = {})
      : timers_(timers), arguments_(arguments),
        provider_cancellation_(std::move(provider_cancellation)),
        command_id_(std::move(command_id)), hooks_(std::move(hooks)),
        capabilities_(std::move(capabilities)) {}

  ~Execution() {
    if (detection_overlay_active_ && capabilities_.clear_detection_overlay) {
      try { capabilities_.clear_detection_overlay(); } catch (...) {}
    }
  }

  const nlohmann::json& arguments() const { return arguments_; }
  const std::string& command_id() const { return command_id_; }
  const std::string& execution_id() const { return hooks_.execution_id; }
  const std::optional<ProcessHandle>& parent_process() const { return hooks_.parent_process; }
  bool cancelled() const {
    return provider_cancellation_.IsCancelled() || (hooks_.cancelled && hooks_.cancelled());
  }
  SuspendFor DelayFor(std::chrono::milliseconds duration) const {
    return SuspendFor(timers_, provider_cancellation_, duration);
  }

  Result<void> Checkpoint(skills::Checkpoint checkpoint) const;
  Result<void> DiagnosticLog(bool enabled = true) const;
  Result<void> PublishState(nlohmann::json state) const;
  Result<void> PublishRelatedProcessState(nlohmann::json state) const;
  Result<std::optional<RetainedExecutionState>> ReadRelatedProcessState(
      const ProcessHandle&) const;
  Result<std::optional<RetainedExecutionState>> WaitRelatedProcessState(
      const ProcessHandle&, std::uint64_t after_revision,
      std::chrono::milliseconds timeout) const;
  Result<std::optional<Signal>> ReceiveSignal(std::chrono::milliseconds timeout) const;
  Result<void> EmitSignal(std::string name, nlohmann::json payload) const;
  Result<ProcessHandle> Spawn(const std::string& name, const std::string& version,
                              nlohmann::json arguments, const std::string& request_id,
                              std::chrono::milliseconds timeout) const;
  Result<void> Send(const ProcessHandle&, const std::string& channel, nlohmann::json payload,
                    const std::string& request_id, std::chrono::milliseconds ttl) const;
  Result<IpcMessage> Receive(const std::string& channel,
                             std::chrono::milliseconds timeout) const;
  Result<IpcMessage> Request(const ProcessHandle&, const std::string& channel,
                             nlohmann::json payload, const std::string& request_id,
                             std::chrono::milliseconds timeout) const;
  Result<ProcessExit> AwaitExit(const ProcessHandle&, std::chrono::milliseconds timeout) const;
  Result<void> Terminate(const ProcessHandle&) const;
  Result<PrimitiveOperation> StartPrimitive(const PrimitiveReference&, const std::string& device_id,
                                             nlohmann::json arguments) const;
  Result<std::optional<nlohmann::json>> WaitPrimitive(
      const PrimitiveOperation&, std::chrono::milliseconds timeout) const;
  Result<void> CancelPrimitive(const PrimitiveOperation&) const;
  Result<NavigationCancelStatus> CancelNavigationCommand(const std::string& command_id,
                                                         const std::string& base_device_id) const;
  Result<nlohmann::json> ReadResource(const std::string& uri) const;
  Result<ResourceSubscription> OpenResourceSubscription(const std::string& uri) const;
  Result<ResourceHistory> ReadResourceHistory(const ResourceSubscription&,
                                              const ResourceSelector&) const;
  Result<ResourceWait> WaitResource(const ResourceSubscription&, std::uint64_t after_sequence,
                                    std::chrono::milliseconds timeout) const;
  Result<void> CloseResourceSubscription(ResourceSubscription&) const;
  Result<void> LogDetectionAttachment(nlohmann::json observation) const;
  Result<void> SetDetectionOverlay(nlohmann::json correction) const;
  Result<void> ClearDetectionOverlay() const;
  Result<std::string> DefaultObjectCamera() const;
  Result<nlohmann::json> ObserveObjects(const std::string& camera_device_id) const;
  Result<nlohmann::json> PlanGrasp(nlohmann::json request) const;
  Result<nlohmann::json> GraspObserveJoints() const;
  Result<std::string> GraspWristCamera() const;
  Result<nlohmann::json> GraspWorkspaceBounds() const;
  Result<nlohmann::json> ObserveObjectsFresh(const std::string& camera_device_id,
                                             std::chrono::steady_clock::time_point after) const;
  Result<InferenceOperation> StartInference(InferenceRequest request) const;
  Result<std::optional<InferenceResult>> WaitInference(
      const InferenceOperation&, std::chrono::milliseconds timeout) const;
  Result<void> CancelInference(const InferenceOperation&) const;

 private:
  eos::lib::TimerService& timers_;
  nlohmann::json arguments_;
  eos::lib::CancellationToken provider_cancellation_;
  std::string command_id_;
  ExecutionHooks hooks_;
  CapabilityHooks capabilities_;
  mutable bool detection_overlay_active_{false};
};

using Handler = std::function<Task<Outcome>(Execution&)>;

struct Definition {
  Metadata metadata;
  Handler handler;
};

}  // namespace eos::runtime::skills::builtin

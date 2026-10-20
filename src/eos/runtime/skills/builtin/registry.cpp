#include "src/eos/runtime/skills/builtin/registry.h"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <condition_variable>
#include <deque>
#include <future>
#include <set>
#include <thread>
#include <unordered_map>

#include "src/eos/lib/uuid.h"
#include "src/eos/runtime/skills/core/admission.h"

namespace eos::runtime::skills::builtin {
namespace detail {
struct ResourceSubscriptionState {
  RuntimeCapabilities::ResourceAccess access;
};
}  // namespace detail
namespace {
constexpr std::size_t kMaxResourceRange = 256;
constexpr std::size_t kMaxResourceTextBytes = 4096;
constexpr auto kMaxResourceWait = std::chrono::minutes(1);

struct DeviceLease {
  std::function<void()> release;
  ~DeviceLease() {
    if (release) release();
  }
};

bool DeclaresResource(const Metadata& metadata, const std::string& uri) {
  return std::any_of(metadata.resources.begin(), metadata.resources.end(), [&](const auto& resource) {
    return ResourceTemplateMatches(resource.uri_template, uri);
  });
}

bool ValidResourceUriBounds(const std::string& uri) {
  return !uri.empty() && uri.size() <= kMaxResourceTextBytes;
}

void ValidateSelector(const ResourceSelector& selector) {
  switch (selector.kind) {
    case ResourceSelectorKind::kExactSequence:
      if (selector.first_sequence == 0)
        throw std::invalid_argument("exact resource sequence must be above zero");
      break;
    case ResourceSelectorKind::kSequenceRange:
      if (selector.first_sequence == 0 || selector.first_sequence > selector.last_sequence)
        throw std::invalid_argument("invalid resource sequence range");
      if (selector.limit == 0 || selector.limit > kMaxResourceRange)
        throw std::invalid_argument("resource range limit must be between 1 and 256");
      break;
    case ResourceSelectorKind::kTimeRange:
      if (selector.first_time > selector.last_time)
        throw std::invalid_argument("invalid resource time range");
      if (selector.limit == 0 || selector.limit > kMaxResourceRange)
        throw std::invalid_argument("resource range limit must be between 1 and 256");
      break;
    case ResourceSelectorKind::kLatest:
    case ResourceSelectorKind::kExactTime:
    case ResourceSelectorKind::kBefore:
    case ResourceSelectorKind::kAfter:
    case ResourceSelectorKind::kNearest:
      break;
    default:
      throw std::invalid_argument("invalid resource selector kind");
  }
}

std::unique_ptr<nlohmann::json_schema::json_validator> Compile(const nlohmann::json& schema,
                                                               const char* field) {
  if (!schema.is_object()) throw std::invalid_argument(std::string(field) + " must be an object");
  auto validator = std::make_unique<nlohmann::json_schema::json_validator>(
      nullptr, nlohmann::json_schema::default_string_format_check);
  try {
    validator->set_root_schema(schema);
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string("invalid ") + field + ": " + error.what());
  }
  return validator;
}

RunResult ToRunResult(Outcome outcome) {
  RunResult result;
  result.status = outcome.status;
  result.accepted = outcome.accepted;
  result.reason_code = std::move(outcome.reason_code);
  result.result = std::move(outcome.result);
  result.message = std::move(outcome.message);
  return result;
}
}  // namespace

struct Registry::OperationExecutor {
  struct Operation {
    std::string owner;
    std::shared_future<nlohmann::json> future;
    std::function<void()> cancel;
    bool cancel_requested{false};
  };
  OperationExecutor() {
    for (std::size_t i = 0; i < 8; ++i) workers.emplace_back([this] { Run(); });
  }
  ~OperationExecutor() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    changed.notify_all();
    for (auto& worker : workers) if (worker.joinable()) worker.join();
  }
  Result<PrimitiveOperation> Start(std::string owner, std::function<nlohmann::json()> work,
                                   std::function<void()> cancel) {
    std::lock_guard lock(mutex);
    if (operations.size() >= 256 || owner_counts[owner] >= 64)
      return Result<PrimitiveOperation>::Failure({"operation_limit", "primitive operation limit reached"});
    const std::string id = eos::lib::MintUuidV7();
    auto task = std::make_shared<std::packaged_task<nlohmann::json()>>(std::move(work));
    operations.emplace(id, Operation{owner, task->get_future().share(), std::move(cancel), false});
    ++owner_counts[owner];
    queue.push_back([task] { (*task)(); });
    changed.notify_one();
    return Result<PrimitiveOperation>::Success({id});
  }
  Result<std::optional<nlohmann::json>> Wait(const PrimitiveOperation& operation,
                                              std::chrono::milliseconds timeout) {
    std::shared_future<nlohmann::json> future;
    {
      std::lock_guard lock(mutex);
      const auto found = operations.find(operation.operation_id);
      if (found == operations.end())
        return Result<std::optional<nlohmann::json>>::Failure({"operation_not_found", "primitive operation not found"});
      future = found->second.future;
    }
    if (future.wait_for(timeout) != std::future_status::ready)
      return Result<std::optional<nlohmann::json>>::Success(std::nullopt);
    try {
      nlohmann::json result = future.get();
      Finish(operation.operation_id);
      return Result<std::optional<nlohmann::json>>::Success(std::move(result));
    } catch (const std::exception& error) {
      Finish(operation.operation_id);
      return Result<std::optional<nlohmann::json>>::Failure({"primitive_failed", error.what()});
    }
  }
  Result<void> Cancel(const PrimitiveOperation& operation) {
    std::function<void()> cancel;
    {
      std::lock_guard lock(mutex);
      const auto found = operations.find(operation.operation_id);
      if (found == operations.end()) return Result<void>::Failure({"operation_not_found", "primitive operation not found"});
      if (found->second.cancel_requested) return Result<void>::Success();
      found->second.cancel_requested = true;
      cancel = found->second.cancel;
    }
    try { cancel(); } catch (const std::exception& error) {
      return Result<void>::Failure({"cancel_failed", error.what()});
    }
    return Result<void>::Success();
  }
  void Cleanup(const std::string& owner) {
    std::vector<std::pair<PrimitiveOperation, std::shared_future<nlohmann::json>>> owned;
    {
      std::lock_guard lock(mutex);
      for (const auto& [id, operation] : operations) {
        if (operation.owner == owner) owned.push_back({PrimitiveOperation{id}, operation.future});
      }
    }
    for (const auto& [operation, future] : owned) {
      if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        (void)Cancel(operation);
      future.wait();
      Finish(operation.operation_id);
    }
  }
  void Finish(const std::string& id) {
    std::lock_guard lock(mutex);
    const auto found = operations.find(id);
    if (found == operations.end()) return;
    if (--owner_counts[found->second.owner] == 0) owner_counts.erase(found->second.owner);
    operations.erase(found);
  }
  void Run() {
    while (true) {
      std::function<void()> work;
      {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return stopping || !queue.empty(); });
        if (stopping && queue.empty()) return;
        work = std::move(queue.front()); queue.pop_front();
      }
      work();
    }
  }
  std::mutex mutex;
  std::condition_variable changed;
  bool stopping{false};
  std::deque<std::function<void()>> queue;
  std::unordered_map<std::string, Operation> operations;
  std::unordered_map<std::string, std::size_t> owner_counts;
  std::vector<std::thread> workers;
};

Registry::Registry(eos::lib::TimerService& timers, RuntimeCapabilities capabilities)
    : timers_(timers),
      capabilities_(std::move(capabilities)),
      operations_(std::make_unique<OperationExecutor>()) {}

Registry::~Registry() {
  lifetime_cancellation_.Cancel();
  std::vector<std::shared_ptr<Task<Outcome>::Started>> tasks;
  {
    std::unique_lock lock(tasks_mutex_);
    tasks_changed_.wait(lock, [&] { return active_async_ == 0; });
    tasks = std::move(tasks_);
  }
  for (const auto& task : tasks) task->Wait();
}

CapabilityHooks Registry::BindCapabilities(
    const Entry& entry, const std::string& command_id, const std::string& execution_id,
    Cancelled cancelled,
    std::function<std::function<void()>(const std::string&)> acquire_device) const {
  CapabilityHooks hooks;
  hooks.start_primitive = [this, &entry, command_id, cancelled, acquire_device](
                              const PrimitiveReference& primitive, const std::string& device,
                              nlohmann::json arguments) {
    if (std::find(entry.definition.metadata.primitives.begin(), entry.definition.metadata.primitives.end(), primitive) ==
        entry.definition.metadata.primitives.end())
      return Result<PrimitiveOperation>::Failure({"capability_undeclared", "primitive is not declared"});
    if (!capabilities_.execute_primitive)
      return Result<PrimitiveOperation>::Failure({"capability_unavailable", "primitive capability is unavailable"});
    const std::string operation_command = command_id + ":" + eos::lib::MintUuidV7();
    auto cancel_requested = std::make_shared<std::atomic<bool>>(false);
    auto started = operations_->Start(command_id,
      [capabilities = capabilities_, primitive, device, arguments = std::move(arguments),
       operation_command, cancelled, acquire_device, cancel_requested]() mutable {
        const auto operation_cancelled = [cancelled, cancel_requested] {
          return cancel_requested->load() || (cancelled && cancelled());
        };
        const auto cancelled_ack = [] {
          return nlohmann::json{{"accepted", true},
                                {"result", {{"status", "cancelled"},
                                            {"reason", "primitive operation was cancelled before release"}}}};
        };
        if (operation_cancelled()) return cancelled_ack();
        DeviceLease lease;
        if (acquire_device) {
          lease.release = acquire_device(device);
          if (!lease.release) return cancelled_ack();
        }
        if (operation_cancelled()) return cancelled_ack();
        return capabilities.execute_primitive(primitive, device, std::move(arguments),
                                              operation_command, operation_cancelled);
      }, [capabilities = capabilities_, operation_command, cancel_requested] {
        cancel_requested->store(true);
        if (capabilities.cancel_primitive) capabilities.cancel_primitive(operation_command);
      });
    if (started) started.value().command_id = operation_command;
    return started;
  };
  hooks.wait_primitive = [this](const PrimitiveOperation& operation, std::chrono::milliseconds timeout) {
    return operations_->Wait(operation, timeout);
  };
  hooks.cancel_primitive = [this](const PrimitiveOperation& operation) { return operations_->Cancel(operation); };
  hooks.cancel_navigation_command = [this, &entry](const std::string& command,
                                                    const std::string& device) {
    if (std::find(entry.definition.metadata.primitives.begin(),
                  entry.definition.metadata.primitives.end(),
                  PrimitiveReference{"base.navigate", "1.0.0"}) ==
        entry.definition.metadata.primitives.end())
      return Result<NavigationCancelStatus>::Failure(
          {"capability_undeclared", "base.navigate is not declared"});
    if (command.empty() || device.empty())
      return Result<NavigationCancelStatus>::Failure(
          {"invalid_argument", "command_id and base_device_id are required"});
    if (!capabilities_.cancel_navigation_command)
      return Result<NavigationCancelStatus>::Failure(
          {"capability_unavailable", "navigation cancellation is unavailable"});
    try {
      return Result<NavigationCancelStatus>::Success(
          capabilities_.cancel_navigation_command(command, device));
    } catch (const std::exception& error) {
      return Result<NavigationCancelStatus>::Failure(
          {"navigation_cancel_failed", error.what()});
    }
  };
  hooks.read_resource = [this, &entry](const std::string& uri) {
    if (!ValidResourceUriBounds(uri))
      return Result<nlohmann::json>::Failure(
          {"invalid_resource_uri", "resource URI must contain between 1 and 4096 bytes"});
    if (!DeclaresResource(entry.definition.metadata, uri))
      return Result<nlohmann::json>::Failure({"capability_undeclared", "resource is not declared"});
    if (!capabilities_.read_resource) return Result<nlohmann::json>::Failure({"capability_unavailable", "resource capability is unavailable"});
    try { return Result<nlohmann::json>::Success(capabilities_.read_resource(uri)); }
    catch (const std::exception& error) { return Result<nlohmann::json>::Failure({"resource_read_failed", error.what()}); }
  };
  hooks.open_resource_subscription = [this, &entry](const std::string& uri) {
    if (!ValidResourceUriBounds(uri))
      return Result<ResourceSubscription>::Failure(
          {"invalid_resource_uri", "resource URI must contain between 1 and 4096 bytes"});
    if (!DeclaresResource(entry.definition.metadata, uri))
      return Result<ResourceSubscription>::Failure(
          {"capability_undeclared", "resource is not declared"});
    if (!capabilities_.open_resource_subscription)
      return Result<ResourceSubscription>::Failure(
          {"capability_unavailable", "resource subscriptions are unavailable"});
    try {
      auto access = capabilities_.open_resource_subscription(uri);
      if (!access.read_history || !access.wait_publication)
        return Result<ResourceSubscription>::Failure(
            {"capability_unavailable", "resource subscription is incomplete"});
      return Result<ResourceSubscription>::Success(ResourceSubscription(
          std::make_shared<detail::ResourceSubscriptionState>(
              detail::ResourceSubscriptionState{std::move(access)})));
    } catch (const std::exception& error) {
      return Result<ResourceSubscription>::Failure({"resource_open_failed", error.what()});
    }
  };
  hooks.read_resource_history = [](const ResourceSubscription& subscription,
                                   const ResourceSelector& selector) {
    if (!subscription.state_)
      return Result<ResourceHistory>::Failure(
          {"subscription_closed", "resource subscription is closed"});
    try {
      ValidateSelector(selector);
      return Result<ResourceHistory>::Success(subscription.state_->access.read_history(selector));
    } catch (const std::exception& error) {
      return Result<ResourceHistory>::Failure({"resource_query_failed", error.what()});
    }
  };
  hooks.wait_resource = [cancelled](const ResourceSubscription& subscription,
                                    std::uint64_t after_sequence,
                                    std::chrono::milliseconds timeout) {
    if (!subscription.state_)
      return Result<ResourceWait>::Failure(
          {"subscription_closed", "resource subscription is closed"});
    if (timeout < std::chrono::milliseconds::zero() || timeout > kMaxResourceWait)
      return Result<ResourceWait>::Failure(
          {"invalid_resource_wait", "resource wait timeout must be between 0 and 60000 ms"});
    try {
      return Result<ResourceWait>::Success(subscription.state_->access.wait_publication(
          after_sequence, timeout, cancelled));
    } catch (const std::exception& error) {
      return Result<ResourceWait>::Failure({"resource_wait_failed", error.what()});
    }
  };
  hooks.close_resource_subscription = [](ResourceSubscription& subscription) {
    subscription.state_.reset();
    return Result<void>::Success();
  };
  hooks.log_detection_attachment = [this](nlohmann::json observation) {
    if (!capabilities_.log_detection_attachment)
      return Result<void>::Failure(
          {"capability_unavailable", "detection attachment logging is unavailable"});
    try {
      capabilities_.log_detection_attachment(std::move(observation));
      return Result<void>::Success();
    } catch (const std::exception& error) {
      return Result<void>::Failure({"attachment_log_failed", error.what()});
    }
  };
  if (entry.definition.metadata.name == "observe_human") {
    hooks.set_detection_overlay = [this, execution_id](nlohmann::json correction) {
      if (!capabilities_.set_detection_overlay)
        return Result<void>::Failure({"capability_unavailable", "detection overlay is unavailable"});
      try {
        capabilities_.set_detection_overlay(execution_id, std::move(correction));
        return Result<void>::Success();
      } catch (const std::exception& error) {
        return Result<void>::Failure({"detection_overlay_failed", error.what()});
      }
    };
    hooks.clear_detection_overlay = [this, execution_id]() {
      if (!capabilities_.clear_detection_overlay)
        return Result<void>::Failure({"capability_unavailable", "detection overlay is unavailable"});
      try {
        capabilities_.clear_detection_overlay(execution_id);
        return Result<void>::Success();
      } catch (const std::exception& error) {
        return Result<void>::Failure({"detection_overlay_failed", error.what()});
      }
    };
  }
  hooks.default_object_camera = [this]() {
    if (!capabilities_.default_object_camera)
      return Result<std::string>::Failure({"capability_unavailable", "default object camera is unavailable"});
    try {
      return Result<std::string>::Success(capabilities_.default_object_camera());
    } catch (const std::exception& error) {
      return Result<std::string>::Failure({"camera_unavailable", error.what()});
    }
  };
  hooks.observe_objects = [this, cancelled](const std::string& camera_device_id) {
    if (!capabilities_.observe_objects)
      return Result<nlohmann::json>::Failure({"capability_unavailable", "object observation is unavailable"});
    if (camera_device_id.empty())
      return Result<nlohmann::json>::Failure({"invalid_camera", "camera_device_id is required"});
    try {
      return Result<nlohmann::json>::Success(capabilities_.observe_objects(camera_device_id, cancelled));
    } catch (const std::exception& error) {
      return Result<nlohmann::json>::Failure({"object_observation_failed", error.what()});
    }
  };
  hooks.plan_grasp = [this, cancelled](nlohmann::json request) {
    if (!capabilities_.plan_grasp)
      return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp planner is unavailable"});
    try {
      return Result<nlohmann::json>::Success(capabilities_.plan_grasp(std::move(request), cancelled));
    } catch (const std::exception& error) {
      return Result<nlohmann::json>::Failure({"grasp_plan_failed", error.what()});
    }
  };
  hooks.grasp_observe_joints = [this]() {
    if (!capabilities_.grasp_observe_joints)
      return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp observation pose is unavailable"});
    try {
      return Result<nlohmann::json>::Success(capabilities_.grasp_observe_joints());
    } catch (const std::exception& error) {
      return Result<nlohmann::json>::Failure({"grasp_observe_pose_failed", error.what()});
    }
  };
  hooks.grasp_wrist_camera = [this]() {
    if (!capabilities_.grasp_wrist_camera)
      return Result<std::string>::Failure({"capability_unavailable", "grasp wrist camera is unavailable"});
    try { return Result<std::string>::Success(capabilities_.grasp_wrist_camera()); }
    catch (const std::exception& error) {
      return Result<std::string>::Failure({"grasp_camera_failed", error.what()});
    }
  };
  hooks.grasp_workspace_bounds = [this]() {
    if (!capabilities_.grasp_workspace_bounds)
      return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp workspace is unavailable"});
    try { return Result<nlohmann::json>::Success(capabilities_.grasp_workspace_bounds()); }
    catch (const std::exception& error) {
      return Result<nlohmann::json>::Failure({"grasp_workspace_failed", error.what()});
    }
  };
  hooks.observe_objects_fresh = [this, cancelled](const std::string& camera_device_id,
                                                    std::chrono::steady_clock::time_point after) {
    if (!capabilities_.observe_objects_fresh)
      return Result<nlohmann::json>::Failure({"capability_unavailable", "fresh object observation is unavailable"});
    try {
      return Result<nlohmann::json>::Success(
          capabilities_.observe_objects_fresh(camera_device_id, after, cancelled));
    } catch (const std::exception& error) {
      return Result<nlohmann::json>::Failure({"object_observation_failed", error.what()});
    }
  };
  hooks.start_inference = [this, &entry, command_id, cancelled](InferenceRequest request) {
    if (std::find(entry.definition.metadata.inference_authorities.begin(),
                  entry.definition.metadata.inference_authorities.end(), request.authority) ==
        entry.definition.metadata.inference_authorities.end())
      return Result<InferenceOperation>::Failure(
          {"capability_undeclared", "inference authority is not declared"});
    if (!capabilities_.infer_once)
      return Result<InferenceOperation>::Failure(
          {"capability_unavailable", "inference capability is unavailable"});
    if (!request.options.is_object())
      return Result<InferenceOperation>::Failure(
          {"invalid_inference_request", "inference options must be an object"});
    const bool has_any_provenance = !request.source.empty() || request.source_sequence != 0 ||
                                    request.source_generation != 0;
    const bool has_exact_provenance = !request.source.empty() && request.source_sequence != 0 &&
                                      request.source_generation != 0;
    const bool has_inline_media = request.options.contains("input") ||
                                  request.options.contains("depth_input");
    if (request.authority == InferenceAuthority::kVad) {
      if (!request.source.empty() || request.source_sequence != 0 ||
          request.source_generation != 0 || !request.options.contains("input"))
        return Result<InferenceOperation>::Failure(
            {"invalid_inference_request",
             "VAD inference requires caller-supplied input and empty source provenance"});
    } else if (request.authority == InferenceAuthority::kChatCompletion) {
      if (has_any_provenance || has_inline_media)
        return Result<InferenceOperation>::Failure(
            {"invalid_inference_request",
             "chat completion requires text options and empty source provenance"});
    } else if (request.authority == InferenceAuthority::kVlm) {
      if (request.options.contains("depth_input") ||
          (has_any_provenance && (!has_exact_provenance || has_inline_media)))
        return Result<InferenceOperation>::Failure(
            {"invalid_inference_request",
             "VLM inference requires text with optional inline image or one exact retained frame"});
    } else if (has_any_provenance == has_inline_media ||
               (has_any_provenance && !has_exact_provenance)) {
      return Result<InferenceOperation>::Failure(
          {"invalid_inference_request",
           "vision inference requires either one exact retained frame or inline media"});
    }
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    auto started = operations_->Start(
        command_id,
        [capabilities = capabilities_, request = std::move(request), cancelled, stopped]() {
          auto combined = [cancelled, stopped] {
            return stopped->load() || (cancelled && cancelled());
          };
          InferenceResult result = capabilities.infer_once(request, std::move(combined));
          if (result.authority != request.authority || result.source != request.source ||
              result.source_sequence != request.source_sequence ||
              result.source_generation != request.source_generation || !result.value.is_object())
            throw std::runtime_error("inference provider returned mismatched provenance");
          return nlohmann::json{{"authority", static_cast<int>(result.authority)},
                                {"source", result.source},
                                {"source_sequence", result.source_sequence},
                                {"source_generation", result.source_generation},
                                {"value", std::move(result.value)}};
        },
        [stopped] { stopped->store(true); });
    if (!started) return Result<InferenceOperation>::Failure(started.error());
    return Result<InferenceOperation>::Success({started.value().operation_id});
  };
  hooks.wait_inference = [this](const InferenceOperation& operation,
                                std::chrono::milliseconds timeout) {
    auto waited = operations_->Wait({operation.operation_id}, timeout);
    if (!waited)
      return Result<std::optional<InferenceResult>>::Failure(waited.error());
    if (!waited.value())
      return Result<std::optional<InferenceResult>>::Success(std::nullopt);
    const auto& encoded = *waited.value();
    return Result<std::optional<InferenceResult>>::Success(InferenceResult{
        static_cast<InferenceAuthority>(encoded.at("authority").get<int>()),
        encoded.at("source").get<std::string>(),
        encoded.at("source_sequence").get<std::uint64_t>(),
        encoded.at("source_generation").get<std::uint64_t>(), encoded.at("value")});
  };
  hooks.cancel_inference = [this](const InferenceOperation& operation) {
    return operations_->Cancel({operation.operation_id});
  };
  return hooks;
}

void Registry::ReapCompleted() const {
  std::lock_guard lock(tasks_mutex_);
  for (auto task = tasks_.begin(); task != tasks_.end();) {
    if ((*task)->Done()) task = tasks_.erase(task);
    else ++task;
  }
}

void Registry::Schedule(const Entry& entry, std::function<void()> work) const {
  bool start = false;
  {
    std::lock_guard lock(entry.execution_mutex);
    if (entry.executing) entry.pending.push_back(std::move(work));
    else { entry.executing = true; start = true; }
  }
  if (start) work();
}

void Registry::Release(const Entry& entry) const {
  std::function<void()> next;
  {
    std::lock_guard lock(entry.execution_mutex);
    if (entry.pending.empty()) entry.executing = false;
    else { next = std::move(entry.pending.front()); entry.pending.pop_front(); }
  }
  entry.execution_changed.notify_all();
  if (next) next();
}

void Registry::FinishAsync() const {
  std::lock_guard lock(tasks_mutex_);
  --active_async_;
  tasks_changed_.notify_all();
}

Outcome Outcome::Completed(nlohmann::json result) {
  return {RunResult::Status::kCompleted, true, {}, std::move(result), {}};
}
Outcome Outcome::Rejected(std::string code, std::string message) {
  return {RunResult::Status::kCompleted, false, std::move(code), nlohmann::json::object(),
          std::move(message)};
}
Outcome Outcome::InvalidArgument(std::string code, std::string message) {
  return {RunResult::Status::kInvalidArgument, false, std::move(code), nlohmann::json::object(),
          std::move(message)};
}
Outcome Outcome::Cancelled(std::string code, std::string message) {
  return {RunResult::Status::kCancelled, true, std::move(code), nlohmann::json::object(),
          std::move(message)};
}
Outcome Outcome::Failed(std::string code, std::string message) {
  return {RunResult::Status::kFailed, true, std::move(code), nlohmann::json::object(),
          std::move(message)};
}

Result<void> Execution::Checkpoint(skills::Checkpoint checkpoint) const {
  if (!hooks_.emit_checkpoint)
    return Result<void>::Failure({"capability_unavailable", "checkpoint hook is unavailable"});
  if (checkpoint.fields.is_null()) checkpoint.fields = nlohmann::json::object();
  if (!hooks_.emit_checkpoint(checkpoint))
    return Result<void>::Failure({"checkpoint_rejected", "checkpoint was rejected"});
  return Result<void>::Success();
}

Result<void> Execution::DiagnosticLog(bool enabled) const {
  if (!hooks_.diagnostic_log)
    return Result<void>::Failure({"capability_unavailable", "diagnostic log hook is unavailable"});
  hooks_.diagnostic_log(enabled);
  return Result<void>::Success();
}

Result<void> Execution::PublishState(nlohmann::json state) const {
  if (!hooks_.publish_execution_state)
    return Result<void>::Failure({"capability_unavailable", "execution state hook is unavailable"});
  try {
    if (!hooks_.publish_execution_state(std::move(state)))
      return Result<void>::Failure({"state_rejected", "execution state was rejected"});
    return Result<void>::Success();
  } catch (const std::exception& error) {
    return Result<void>::Failure({"state_publish_failed", error.what()});
  }
}

Result<void> Execution::PublishRelatedProcessState(nlohmann::json state) const {
  if (!hooks_.publish_related_process_state)
    return Result<void>::Failure(
        {"capability_unavailable", "related process state hook is unavailable"});
  try {
    if (!hooks_.publish_related_process_state(std::move(state)))
      return Result<void>::Failure(
          {"state_rejected", "related process state was rejected"});
    return Result<void>::Success();
  } catch (const std::exception& error) {
    return Result<void>::Failure({"state_publish_failed", error.what()});
  }
}

Result<std::optional<RetainedExecutionState>> Execution::ReadRelatedProcessState(
    const ProcessHandle& process) const {
  if (!hooks_.read_related_process_state)
    return Result<std::optional<RetainedExecutionState>>::Failure(
        {"capability_unavailable", "related process state read hook is unavailable"});
  try {
    return Result<std::optional<RetainedExecutionState>>::Success(
        hooks_.read_related_process_state(process));
  } catch (const std::exception& error) {
    return Result<std::optional<RetainedExecutionState>>::Failure(
        {"state_read_failed", error.what()});
  }
}

Result<std::optional<RetainedExecutionState>> Execution::WaitRelatedProcessState(
    const ProcessHandle& process, std::uint64_t after_revision,
    std::chrono::milliseconds timeout) const {
  if (!hooks_.wait_related_process_state)
    return Result<std::optional<RetainedExecutionState>>::Failure(
        {"capability_unavailable", "related process state wait hook is unavailable"});
  try {
    return Result<std::optional<RetainedExecutionState>>::Success(
        hooks_.wait_related_process_state(process, after_revision, timeout));
  } catch (const std::exception& error) {
    return Result<std::optional<RetainedExecutionState>>::Failure(
        {"state_wait_failed", error.what()});
  }
}

Result<std::optional<Signal>> Execution::ReceiveSignal(std::chrono::milliseconds timeout) const {
  if (!hooks_.receive_signal)
    return Result<std::optional<Signal>>::Failure(
        {"capability_unavailable", "signal receive hook is unavailable"});
  try {
    return Result<std::optional<Signal>>::Success(hooks_.receive_signal(timeout));
  } catch (const std::exception& error) {
    return Result<std::optional<Signal>>::Failure({"signal_receive_failed", error.what()});
  }
}

Result<void> Execution::EmitSignal(std::string name, nlohmann::json payload) const {
  if (!hooks_.emit_signal)
    return Result<void>::Failure({"capability_unavailable", "signal emit hook is unavailable"});
  try {
    if (!hooks_.emit_signal({std::move(name), std::move(payload)}))
      return Result<void>::Failure({"signal_rejected", "signal was rejected"});
    return Result<void>::Success();
  } catch (const std::exception& error) {
    return Result<void>::Failure({"signal_emit_failed", error.what()});
  }
}

Result<ProcessHandle> Execution::Spawn(const std::string& name, const std::string& version,
                                       nlohmann::json arguments,
                                       const std::string& request_id,
                                       std::chrono::milliseconds timeout) const {
  if (!hooks_.spawn)
    return Result<ProcessHandle>::Failure({"capability_unavailable", "spawn hook is unavailable"});
  try {
    return Result<ProcessHandle>::Success(
        hooks_.spawn(name, version, std::move(arguments), request_id, timeout));
  } catch (const std::exception& error) {
    return Result<ProcessHandle>::Failure({"spawn_failed", error.what()});
  }
}

Result<void> Execution::Send(const ProcessHandle& process, const std::string& channel,
                             nlohmann::json payload, const std::string& request_id,
                             std::chrono::milliseconds ttl) const {
  if (!hooks_.send_message)
    return Result<void>::Failure({"capability_unavailable", "send hook is unavailable"});
  try {
    if (!hooks_.send_message(process, channel, std::move(payload), request_id, ttl))
      return Result<void>::Failure({"send_rejected", "message was rejected"});
    return Result<void>::Success();
  } catch (const std::exception& error) {
    return Result<void>::Failure({"send_failed", error.what()});
  }
}

Result<IpcMessage> Execution::Receive(const std::string& channel,
                                      std::chrono::milliseconds timeout) const {
  if (!hooks_.receive_message)
    return Result<IpcMessage>::Failure({"capability_unavailable", "receive hook is unavailable"});
  try {
    auto message = hooks_.receive_message(channel, timeout);
    if (!message) return Result<IpcMessage>::Failure({"receive_timeout", "no message received"});
    return Result<IpcMessage>::Success(std::move(*message));
  } catch (const std::exception& error) {
    return Result<IpcMessage>::Failure({"receive_failed", error.what()});
  }
}

Result<IpcMessage> Execution::Request(const ProcessHandle& process, const std::string& channel,
                                       nlohmann::json payload, const std::string& request_id,
                                       std::chrono::milliseconds timeout) const {
  auto sent = Send(process, channel, std::move(payload), request_id, timeout);
  if (!sent) return Result<IpcMessage>::Failure(sent.error());
  if (!hooks_.receive_reply)
    return Result<IpcMessage>::Failure(
        {"capability_unavailable", "correlated receive hook is unavailable"});
  try {
    auto message = hooks_.receive_reply(process, channel, request_id, timeout);
    if (!message) return Result<IpcMessage>::Failure({"receive_timeout", "no reply received"});
    return Result<IpcMessage>::Success(std::move(*message));
  } catch (const std::exception& error) {
    return Result<IpcMessage>::Failure({"receive_failed", error.what()});
  }
}

Result<ProcessExit> Execution::AwaitExit(const ProcessHandle& process,
                                         std::chrono::milliseconds timeout) const {
  if (!hooks_.await_exit)
    return Result<ProcessExit>::Failure({"capability_unavailable", "await hook is unavailable"});
  try {
    auto result = hooks_.await_exit(process, timeout);
    if (!result) return Result<ProcessExit>::Failure({"await_timeout", "child is still running"});
    return Result<ProcessExit>::Success(std::move(*result));
  } catch (const std::exception& error) {
    return Result<ProcessExit>::Failure({"await_failed", error.what()});
  }
}

Result<void> Execution::Terminate(const ProcessHandle& process) const {
  if (!hooks_.terminate_process)
    return Result<void>::Failure({"capability_unavailable", "terminate hook is unavailable"});
  try {
    if (!hooks_.terminate_process(process))
      return Result<void>::Failure({"process_not_found", "child process was not found"});
    return Result<void>::Success();
  } catch (const std::exception& error) {
    return Result<void>::Failure({"terminate_failed", error.what()});
  }
}

Result<PrimitiveOperation> Execution::StartPrimitive(const PrimitiveReference& primitive,
                                                       const std::string& device_id,
                                                       nlohmann::json arguments) const {
  if (!capabilities_.start_primitive)
    return Result<PrimitiveOperation>::Failure({"capability_unavailable", "primitive start is unavailable"});
  const auto started = std::chrono::steady_clock::now();
  auto result = capabilities_.start_primitive(primitive, device_id, std::move(arguments));
  if (hooks_.record_capability)
    hooks_.record_capability(
        "primitive.call", std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started));
  return result;
}
Result<std::optional<nlohmann::json>> Execution::WaitPrimitive(
    const PrimitiveOperation& operation, std::chrono::milliseconds timeout) const {
  if (!capabilities_.wait_primitive)
    return Result<std::optional<nlohmann::json>>::Failure({"capability_unavailable", "primitive wait is unavailable"});
  return capabilities_.wait_primitive(operation, timeout);
}
Result<void> Execution::CancelPrimitive(const PrimitiveOperation& operation) const {
  if (!capabilities_.cancel_primitive)
    return Result<void>::Failure({"capability_unavailable", "primitive cancellation is unavailable"});
  return capabilities_.cancel_primitive(operation);
}
Result<NavigationCancelStatus> Execution::CancelNavigationCommand(
    const std::string& command_id, const std::string& base_device_id) const {
  if (!capabilities_.cancel_navigation_command)
    return Result<NavigationCancelStatus>::Failure(
        {"capability_unavailable", "navigation cancellation is unavailable"});
  return capabilities_.cancel_navigation_command(command_id, base_device_id);
}
Result<nlohmann::json> Execution::ReadResource(const std::string& uri) const {
  if (!capabilities_.read_resource)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "resource read is unavailable"});
  const auto started = std::chrono::steady_clock::now();
  auto result = capabilities_.read_resource(uri);
  if (hooks_.record_capability)
    hooks_.record_capability(
        "resource.read", std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started));
  return result;
}
Result<ResourceSubscription> Execution::OpenResourceSubscription(const std::string& uri) const {
  if (!capabilities_.open_resource_subscription)
    return Result<ResourceSubscription>::Failure(
        {"capability_unavailable", "resource subscription open is unavailable"});
  return capabilities_.open_resource_subscription(uri);
}
Result<ResourceHistory> Execution::ReadResourceHistory(
    const ResourceSubscription& subscription, const ResourceSelector& selector) const {
  if (!capabilities_.read_resource_history)
    return Result<ResourceHistory>::Failure(
        {"capability_unavailable", "resource history read is unavailable"});
  return capabilities_.read_resource_history(subscription, selector);
}
Result<ResourceWait> Execution::WaitResource(const ResourceSubscription& subscription,
                                             std::uint64_t after_sequence,
                                             std::chrono::milliseconds timeout) const {
  if (!capabilities_.wait_resource)
    return Result<ResourceWait>::Failure(
        {"capability_unavailable", "resource wait is unavailable"});
  return capabilities_.wait_resource(subscription, after_sequence, timeout);
}
Result<void> Execution::CloseResourceSubscription(ResourceSubscription& subscription) const {
  if (!capabilities_.close_resource_subscription)
    return Result<void>::Failure(
        {"capability_unavailable", "resource subscription close is unavailable"});
  return capabilities_.close_resource_subscription(subscription);
}
Result<void> Execution::LogDetectionAttachment(nlohmann::json observation) const {
  if (!capabilities_.log_detection_attachment)
    return Result<void>::Failure(
        {"capability_unavailable", "detection attachment logging is unavailable"});
  return capabilities_.log_detection_attachment(std::move(observation));
}
Result<void> Execution::SetDetectionOverlay(nlohmann::json correction) const {
  if (!capabilities_.set_detection_overlay)
    return Result<void>::Failure({"capability_unavailable", "detection overlay is unavailable"});
  auto result = capabilities_.set_detection_overlay(std::move(correction));
  if (result) detection_overlay_active_ = true;
  return result;
}
Result<void> Execution::ClearDetectionOverlay() const {
  if (!capabilities_.clear_detection_overlay)
    return Result<void>::Failure({"capability_unavailable", "detection overlay is unavailable"});
  if (!detection_overlay_active_) return Result<void>::Success();
  auto result = capabilities_.clear_detection_overlay();
  if (result) detection_overlay_active_ = false;
  return result;
}
Result<std::string> Execution::DefaultObjectCamera() const {
  if (!capabilities_.default_object_camera)
    return Result<std::string>::Failure({"capability_unavailable", "default object camera is unavailable"});
  return capabilities_.default_object_camera();
}
Result<nlohmann::json> Execution::ObserveObjects(const std::string& camera_device_id) const {
  if (!capabilities_.observe_objects)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "object observation is unavailable"});
  return capabilities_.observe_objects(camera_device_id);
}
Result<nlohmann::json> Execution::PlanGrasp(nlohmann::json request) const {
  if (!capabilities_.plan_grasp)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp planner is unavailable"});
  return capabilities_.plan_grasp(std::move(request));
}
Result<nlohmann::json> Execution::GraspObserveJoints() const {
  if (!capabilities_.grasp_observe_joints)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp observation pose is unavailable"});
  return capabilities_.grasp_observe_joints();
}
Result<std::string> Execution::GraspWristCamera() const {
  if (!capabilities_.grasp_wrist_camera)
    return Result<std::string>::Failure({"capability_unavailable", "grasp wrist camera is unavailable"});
  return capabilities_.grasp_wrist_camera();
}
Result<nlohmann::json> Execution::GraspWorkspaceBounds() const {
  if (!capabilities_.grasp_workspace_bounds)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "grasp workspace is unavailable"});
  return capabilities_.grasp_workspace_bounds();
}
Result<nlohmann::json> Execution::ObserveObjectsFresh(
    const std::string& camera_device_id, std::chrono::steady_clock::time_point after) const {
  if (!capabilities_.observe_objects_fresh)
    return Result<nlohmann::json>::Failure({"capability_unavailable", "fresh object observation is unavailable"});
  return capabilities_.observe_objects_fresh(camera_device_id, after);
}
Result<InferenceOperation> Execution::StartInference(InferenceRequest request) const {
  if (!capabilities_.start_inference)
    return Result<InferenceOperation>::Failure(
        {"capability_unavailable", "inference start is unavailable"});
  return capabilities_.start_inference(std::move(request));
}
Result<std::optional<InferenceResult>> Execution::WaitInference(
    const InferenceOperation& operation, std::chrono::milliseconds timeout) const {
  if (!capabilities_.wait_inference)
    return Result<std::optional<InferenceResult>>::Failure(
        {"capability_unavailable", "inference wait is unavailable"});
  return capabilities_.wait_inference(operation, timeout);
}
Result<void> Execution::CancelInference(const InferenceOperation& operation) const {
  if (!capabilities_.cancel_inference)
    return Result<void>::Failure(
        {"capability_unavailable", "inference cancellation is unavailable"});
  return capabilities_.cancel_inference(operation);
}

void Registry::Register(Definition definition) {
  if (definition.metadata.name.empty() || definition.metadata.version.empty())
    throw std::invalid_argument("built-in skill identity is required");
  if (!definition.handler) throw std::invalid_argument("built-in skill handler is required");
  for (const auto& channel : definition.metadata.ipc_channels) {
    if (channel.name.empty() || (!channel.can_send && !channel.can_receive))
      throw std::invalid_argument("invalid built-in IPC channel declaration");
    (void)Compile(channel.schema, "IPC channel schema");
  }
  std::set<std::string> resources;
  for (const auto& resource : definition.metadata.resources) {
    if (resource.uri_template.size() > kMaxResourceTextBytes ||
        resource.description.size() > kMaxResourceTextBytes ||
        !ValidResourceTemplate(resource.uri_template))
      throw std::invalid_argument("invalid built-in resource declaration");
    if (!resources.insert(resource.uri_template).second)
      throw std::invalid_argument("duplicate built-in resource declaration");
  }
  auto entry = std::make_unique<Entry>();
  entry->parameters = Compile(definition.metadata.parameters_schema, "parameters schema");
  entry->returns = Compile(definition.metadata.returns_schema, "returns schema");
  if (definition.metadata.execution_state_schema) {
    if (!definition.metadata.execution_state_schema->is_object() ||
        definition.metadata.execution_state_schema->value("type", std::string{}) != "object")
      throw std::invalid_argument("execution state schema must describe an object");
    entry->execution_state =
        Compile(*definition.metadata.execution_state_schema, "execution state schema");
  }
  if (definition.metadata.related_process_state_schema) {
    if (!definition.metadata.related_process_state_schema->is_object() ||
        definition.metadata.related_process_state_schema->value("type", std::string{}) !=
            "object")
      throw std::invalid_argument("related process state schema must describe an object");
    entry->related_process_state = Compile(*definition.metadata.related_process_state_schema,
                                           "related process state schema");
  }
  entry->definition = std::move(definition);
  const Identity identity{entry->definition.metadata.name, entry->definition.metadata.version};
  if (!entries_.emplace(identity, std::move(entry)).second)
    throw std::invalid_argument("duplicate built-in skill: " + identity.first + "@" +
                                identity.second);
}

std::vector<Metadata> Registry::Discover() const {
  std::vector<Metadata> result;
  result.reserve(entries_.size());
  for (const auto& [identity, entry] : entries_) result.push_back(entry->definition.metadata);
  return result;
}

const Metadata* Registry::Find(const std::string& name, const std::string& version) const {
  const auto found = entries_.find({name, version});
  return found == entries_.end() ? nullptr : &found->second->definition.metadata;
}

const Registry::Entry& Registry::Require(const std::string& name, const std::string& version) const {
  const auto found = entries_.find({name, version});
  if (found == entries_.end())
    throw std::invalid_argument("skill is not admitted: " + name + "@" + version);
  return *found->second;
}

void Registry::ValidateArguments(const std::string& name, const std::string& version,
                                 const nlohmann::json& arguments) const {
  if (!arguments.is_object()) throw std::invalid_argument("skill arguments must be an object");
  try {
    Require(name, version).parameters->validate(arguments);
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string("skill arguments violate parameters schema: ") +
                                error.what());
  }
}

RunResult Registry::Execute(const std::string& name, const std::string& version,
                            const nlohmann::json& arguments, const std::string& command_id,
                            Cancelled cancelled) const {
  ExecutionHooks hooks;
  hooks.cancelled = std::move(cancelled);
  return ExecuteWithHooks(name, version, arguments, command_id, std::move(hooks));
}

ExecutionHooks Registry::BindStateValidation(const Entry& entry, ExecutionHooks hooks) const {
  auto publish_execution = std::move(hooks.publish_execution_state);
  hooks.publish_execution_state = [&entry, publish = std::move(publish_execution)](
                                      nlohmann::json state) mutable {
    if (!entry.execution_state || !publish || !state.is_object()) return false;
    entry.execution_state->validate(state);
    return publish(std::move(state));
  };
  auto publish_related = std::move(hooks.publish_related_process_state);
  hooks.publish_related_process_state = [&entry, publish = std::move(publish_related)](
                                            nlohmann::json state) mutable {
    if (!entry.related_process_state || !publish || !state.is_object()) return false;
    entry.related_process_state->validate(state);
    return publish(std::move(state));
  };
  return hooks;
}

RunResult Registry::ExecuteWithHooks(const std::string& name, const std::string& version,
                                     const nlohmann::json& arguments,
                                     const std::string& command_id, ExecutionHooks hooks) const {
  if (command_id.empty())
    return ToRunResult(Outcome::InvalidArgument("missing_command_id", "command_id is required"));
  try {
    ValidateArguments(name, version, arguments);
    const Entry& entry = Require(name, version);
    hooks = BindStateValidation(entry, std::move(hooks));
    auto invoke = [&] {
      const auto provider_cancellation = lifetime_cancellation_.token();
      auto cancelled = [provider_cancellation, requested = hooks.cancelled] {
        return provider_cancellation.IsCancelled() || (requested && requested());
      };
      auto capabilities =
          BindCapabilities(entry, command_id,
                           hooks.execution_id.empty() ? command_id : hooks.execution_id,
                           std::move(cancelled), hooks.acquire_device);
      Execution execution(timers_, arguments, provider_cancellation, command_id, std::move(hooks),
                           std::move(capabilities));
      Outcome outcome;
      try {
        outcome = entry.definition.handler(execution).Get();
      } catch (...) {
        operations_->Cleanup(command_id);
        throw;
      }
      operations_->Cleanup(command_id);
      if (!outcome.result.is_object())
        return ToRunResult(Outcome::Failed("invalid_result", "skill result must be an object"));
      if (outcome.status == RunResult::Status::kCompleted) {
        try {
          entry.returns->validate(outcome.result);
        } catch (const std::exception& error) {
          return ToRunResult(Outcome::Failed("invalid_result", error.what()));
        }
      }
      return ToRunResult(std::move(outcome));
    };
    if (entry.definition.metadata.reentrant) return invoke();
    {
      std::unique_lock lock(entry.execution_mutex);
      entry.execution_changed.wait(lock, [&] { return !entry.executing; });
      entry.executing = true;
    }
    try {
      auto result = invoke();
      Release(entry);
      return result;
    } catch (...) {
      Release(entry);
      throw;
    }
  } catch (const std::invalid_argument& error) {
    return ToRunResult(Outcome::InvalidArgument("invalid_arguments", error.what()));
  } catch (const std::exception& error) {
    return ToRunResult(Outcome::Failed("handler_exception", error.what()));
  } catch (...) {
    return ToRunResult(Outcome::Failed("handler_exception", "unknown handler exception"));
  }
}

void Registry::ExecuteAsync(const std::string& name, const std::string& version,
                            const nlohmann::json& arguments, const std::string& command_id,
                            ExecutionHooks hooks, RunCompletion completion) const {
  if (!completion) throw std::invalid_argument("built-in completion callback is required");
  if (command_id.empty()) {
    completion(ToRunResult(
        Outcome::InvalidArgument("missing_command_id", "command_id is required")));
    return;
  }
  try {
    ValidateArguments(name, version, arguments);
    const Entry& entry = Require(name, version);
    hooks = BindStateValidation(entry, std::move(hooks));
    ReapCompleted();
    {
      std::lock_guard lock(tasks_mutex_);
      ++active_async_;
    }
    const Entry* owned_entry = &entry;
    const bool serialized = !entry.definition.metadata.reentrant;
    auto start = [this, arguments, command_id, hooks = std::move(hooks), owned_entry,
                   serialized, completion = std::move(completion)]() mutable {
      try {
        const auto provider_cancellation = lifetime_cancellation_.token();
        auto cancelled = [provider_cancellation, requested = hooks.cancelled] {
          return provider_cancellation.IsCancelled() || (requested && requested());
        };
        auto capabilities = BindCapabilities(*owned_entry, command_id,
                                             hooks.execution_id.empty() ? command_id : hooks.execution_id,
                                             std::move(cancelled),
                                             hooks.acquire_device);
        auto execution = std::make_shared<Execution>(timers_, arguments, provider_cancellation,
                                                     command_id,
                                                     std::move(hooks), std::move(capabilities));
        auto task = owned_entry->definition.handler(*execution);
        auto running = std::move(task).Start(
        [this, command_id, execution = std::move(execution), owned_entry, serialized,
         completion = std::move(completion)](std::optional<Outcome> outcome,
                                             std::exception_ptr error) mutable {
          auto complete = [&](RunResult result) {
            try { completion(std::move(result)); } catch (...) {}
            if (serialized) Release(*owned_entry);
            FinishAsync();
          };
          try {
            operations_->Cleanup(command_id);
          } catch (const std::exception& exception) {
            complete(ToRunResult(Outcome::Failed("handler_exception", exception.what())));
            return;
          } catch (...) {
            complete(ToRunResult(
                Outcome::Failed("handler_exception", "unknown handler exception")));
            return;
          }
          if (error) {
            try {
              std::rethrow_exception(error);
            } catch (const std::exception& exception) {
              complete(ToRunResult(Outcome::Failed("handler_exception", exception.what())));
            } catch (...) {
              complete(ToRunResult(
                  Outcome::Failed("handler_exception", "unknown handler exception")));
            }
            return;
          }
          if (!outcome) {
            complete(ToRunResult(
                Outcome::Failed("handler_exception", "handler produced no outcome")));
            return;
          }
          if (!outcome->result.is_object()) {
            complete(ToRunResult(
                Outcome::Failed("invalid_result", "skill result must be an object")));
            return;
          }
          if (outcome->status == RunResult::Status::kCompleted) {
            try {
              owned_entry->returns->validate(outcome->result);
            } catch (const std::exception& exception) {
              complete(ToRunResult(Outcome::Failed("invalid_result", exception.what())));
              return;
            }
          }
          complete(ToRunResult(std::move(*outcome)));
        });
        {
          std::lock_guard lock(tasks_mutex_);
          tasks_.push_back(running);
        }
        running->Resume();
      } catch (const std::exception& error) {
        try { completion(ToRunResult(Outcome::Failed("handler_exception", error.what()))); } catch (...) {}
        if (serialized) Release(*owned_entry);
        FinishAsync();
      } catch (...) {
        try { completion(ToRunResult(Outcome::Failed("handler_exception", "unknown handler exception"))); } catch (...) {}
        if (serialized) Release(*owned_entry);
        FinishAsync();
      }
    };
    if (entry.definition.metadata.reentrant) start();
    else Schedule(entry, std::move(start));
  } catch (const std::invalid_argument& error) {
    completion(ToRunResult(Outcome::InvalidArgument("invalid_arguments", error.what())));
  } catch (const std::exception& error) {
    completion(ToRunResult(Outcome::Failed("handler_exception", error.what())));
  } catch (...) {
    completion(ToRunResult(Outcome::Failed("handler_exception", "unknown handler exception")));
  }
}

}  // namespace eos::runtime::skills::builtin

#pragma once

#include <map>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json-schema.hpp>

#include "src/eos/lib/cancellation.h"
#include "src/eos/runtime/skills/builtin/runtime.h"

namespace eos::runtime::skills::builtin {

struct RuntimeCapabilities {
  struct ResourceAccess {
    std::shared_ptr<void> lifetime;
    std::function<ResourceHistory(const ResourceSelector&)> read_history;
    std::function<ResourceWait(std::uint64_t, std::chrono::milliseconds, Cancelled)>
        wait_publication;
  };

  std::function<nlohmann::json(const PrimitiveReference&, const std::string&, nlohmann::json,
                               const std::string&, Cancelled)> execute_primitive;
  std::function<void(const std::string&)> cancel_primitive;
  std::function<NavigationCancelStatus(const std::string&, const std::string&)>
      cancel_navigation_command;
  std::function<nlohmann::json(const std::string&)> read_resource;
  std::function<ResourceAccess(const std::string&)> open_resource_subscription;
  std::function<void(nlohmann::json)> log_detection_attachment;
  std::function<void(const std::string&, nlohmann::json)> set_detection_overlay;
  std::function<void(const std::string&)> clear_detection_overlay;
  std::function<std::string()> default_object_camera;
  std::function<nlohmann::json(const std::string&, Cancelled)> observe_objects;
  std::function<nlohmann::json(nlohmann::json, Cancelled)> plan_grasp;
  std::function<nlohmann::json()> grasp_observe_joints;
  std::function<std::string()> grasp_wrist_camera;
  std::function<nlohmann::json()> grasp_workspace_bounds;
  std::function<nlohmann::json(const std::string&, std::chrono::steady_clock::time_point, Cancelled)> observe_objects_fresh;
  std::function<InferenceResult(const InferenceRequest&, Cancelled)> infer_once;
};

class Registry final : public Provider {
 public:
  explicit Registry(eos::lib::TimerService& timers, RuntimeCapabilities capabilities = {});
  ~Registry() override;
  void Register(Definition definition);

  std::vector<Metadata> Discover() const override;
  const Metadata* Find(const std::string& name, const std::string& version) const override;
  void ValidateArguments(const std::string& name, const std::string& version,
                         const nlohmann::json& arguments) const override;
  RunResult Execute(const std::string& name, const std::string& version,
                    const nlohmann::json& arguments, const std::string& command_id,
                    Cancelled cancelled = {}) const override;
  RunResult ExecuteWithHooks(const std::string& name, const std::string& version,
                             const nlohmann::json& arguments, const std::string& command_id,
                             ExecutionHooks hooks) const override;
  void ExecuteAsync(const std::string& name, const std::string& version,
                    const nlohmann::json& arguments, const std::string& command_id,
                    ExecutionHooks hooks, RunCompletion completion) const override;

 private:
  struct Entry {
    Definition definition;
    std::unique_ptr<nlohmann::json_schema::json_validator> parameters;
    std::unique_ptr<nlohmann::json_schema::json_validator> returns;
    std::unique_ptr<nlohmann::json_schema::json_validator> execution_state;
    std::unique_ptr<nlohmann::json_schema::json_validator> related_process_state;
    mutable std::mutex execution_mutex;
    mutable std::condition_variable execution_changed;
    mutable bool executing{false};
    mutable std::deque<std::function<void()>> pending;
  };
  using Identity = std::pair<std::string, std::string>;
  const Entry& Require(const std::string& name, const std::string& version) const;
  void ReapCompleted() const;
  void Schedule(const Entry&, std::function<void()>) const;
  void Release(const Entry&) const;
  void FinishAsync() const;
  CapabilityHooks BindCapabilities(
      const Entry&, const std::string&, const std::string&, Cancelled,
      std::function<std::function<void()>(const std::string&)> acquire_device) const;
  ExecutionHooks BindStateValidation(const Entry&, ExecutionHooks) const;
  struct OperationExecutor;
  eos::lib::TimerService& timers_;
  std::map<Identity, std::unique_ptr<Entry>> entries_;
  mutable std::mutex tasks_mutex_;
  mutable std::vector<std::shared_ptr<Task<Outcome>::Started>> tasks_;
  mutable std::condition_variable tasks_changed_;
  mutable std::size_t active_async_{0};
  mutable eos::lib::CancellationSource lifetime_cancellation_;
  RuntimeCapabilities capabilities_;
  std::unique_ptr<OperationExecutor> operations_;
};

}  // namespace eos::runtime::skills::builtin

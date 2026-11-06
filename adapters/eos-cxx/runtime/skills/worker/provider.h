#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapters/eos-cxx/runtime/dispatch/dispatcher.h"
#include "adapters/eos-cxx/runtime/skills/core/provider.h"
#include "adapters/eos-cxx/runtime/skills/worker/grants.h"

namespace eos::runtime::kernel {
class Broker;
class Catalog;
}

namespace eos::runtime::skills::worker {

// A package skill provider that launches a fresh worker for each execution.
// Production supplies kernel Broker/Catalog; the resolver supplies admitted
// targets, and Broker alone mints and validates their execution-scoped tokens.
class NativeWorkerProvider final : public Provider {
 public:
  using GrantResolver = std::function<std::vector<Grant>(
      const Metadata&, const nlohmann::json&, const std::string&)>;
  using ResourceReader = std::function<nlohmann::json(const std::string&)>;
  using InferenceRunner = std::function<InferenceResult(const InferenceRequest&, Cancelled)>;
  // Compatibility constructor for isolated tests. Production uses the
  // injected kernel constructor below.
  NativeWorkerProvider(std::filesystem::path worker_executable,
                       std::filesystem::path skill_directory,
                       dispatch::Dispatcher& dispatcher,
                       GrantResolver resolve_grants = {},
                       ResourceReader read_resource = {},
                       InferenceRunner infer = {});
  NativeWorkerProvider(std::filesystem::path worker_executable,
                       std::filesystem::path skill_directory,
                       dispatch::Dispatcher& dispatcher,
                       kernel::Broker& broker, const kernel::Catalog& catalog,
                       GrantResolver resolve_grants,
                       ResourceReader read_resource = {},
                       InferenceRunner infer = {});
  ~NativeWorkerProvider() override;
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
  std::filesystem::path worker_executable_;
  std::filesystem::path skill_directory_;
  dispatch::Dispatcher& dispatcher_;
  kernel::Broker* kernel_broker_{nullptr};
  const kernel::Catalog* kernel_catalog_{nullptr};
  GrantResolver resolve_grants_;
  ResourceReader read_resource_;
  InferenceRunner infer_;
  std::vector<PrimitiveReference> primitives_;
  std::vector<Metadata> catalog_;
  std::map<std::pair<std::string, std::string>, std::size_t> by_identity_;
  // A fixed pool bounds concurrent worker processes and keeps SkillManager's
  // scheduling threads free while a skill awaits hardware completion.
  mutable std::mutex jobs_mutex_;
  mutable std::condition_variable jobs_ready_;
  mutable std::deque<std::function<void()>> jobs_;
  mutable std::vector<std::thread> jobs_workers_;
  mutable bool stopping_{false};
  static constexpr std::size_t kMaxQueuedExecutions = 64;
};

}  // namespace eos::runtime::skills::worker

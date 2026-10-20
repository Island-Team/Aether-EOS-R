#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>
#include <thread>

#include "eos/skill/abi.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/skills/native/runtime.h"

namespace eos::runtime::skills::native::detail {
inline constexpr std::uint32_t kMaxLibraries = 256;
inline constexpr std::uint32_t kMaxSkillsPerLibrary = 256;
inline constexpr std::uint32_t kMaxPrimitivesPerSkill = 64;
inline constexpr std::uint32_t kMaxAsyncPrimitivesPerExecution = 64;
inline constexpr std::uint32_t kMaxResourcesPerSkill = 64;
inline constexpr std::uint32_t kMaxSignalsPerSkill = 64;
inline constexpr std::uint32_t kMaxClaimsPerSkill = 64;
inline constexpr std::uint32_t kMaxSourcesPerSkill = 64;
inline constexpr std::uint32_t kMaxNameBytes = 128;
inline constexpr std::uint32_t kMaxVersionBytes = 128;
inline constexpr std::uint32_t kMaxDescriptionBytes = 4096;
inline constexpr std::uint32_t kMaxJsonBytes = 1024 * 1024;
inline constexpr std::uint32_t kMaxResourceTemplateBytes = 4096;

/// Bounded process-local executor for blocking dispatcher operations. Native
/// skill callbacks enqueue work here instead of creating an unbounded thread
/// for every asynchronous primitive.
class PrimitiveExecutor {
 public:
  explicit PrimitiveExecutor(std::size_t worker_count = 8, std::size_t maximum_outstanding = 256)
      : maximum_outstanding_(maximum_outstanding) {
    for (std::size_t i = 0; i < worker_count; ++i) workers_.emplace_back([this] { Run(); });
  }
  ~PrimitiveExecutor() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    available_.notify_all();
    for (auto& worker : workers_)
      if (worker.joinable()) worker.join();
  }
  PrimitiveExecutor(const PrimitiveExecutor&) = delete;
  PrimitiveExecutor& operator=(const PrimitiveExecutor&) = delete;

  template <class Function>
  auto Submit(Function&& function) -> std::future<std::invoke_result_t<Function>> {
    using Result = std::invoke_result_t<Function>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<Function>(function));
    auto result = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) throw std::runtime_error("primitive executor is stopping");
      if (outstanding_ >= maximum_outstanding_)
        throw std::runtime_error("primitive executor capacity reached");
      ++outstanding_;
      pending_.emplace_back([this, task] {
        (*task)();
        std::lock_guard<std::mutex> lock(mutex_);
        --outstanding_;
      });
    }
    available_.notify_one();
    return result;
  }

 private:
  void Run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
        if (stopping_ && pending_.empty()) return;
        task = std::move(pending_.front());
        pending_.pop_front();
      }
      task();
    }
  }
  const std::size_t maximum_outstanding_;
  std::mutex mutex_;
  std::condition_variable available_;
  std::deque<std::function<void()>> pending_;
  std::vector<std::thread> workers_;
  std::size_t outstanding_{0};
  bool stopping_{false};
};

bool ValidUtf8(const std::string& value);
std::string CopyText(eos_skill_text_v2 text, std::uint32_t maximum, const std::string& field,
                     bool required = true);
eos_skill_text_v2 Borrow(const std::string& text);
bool ValidName(const std::string& value);
bool ValidVersion(const std::string& value);
std::string DlError();

struct Handle {
  void* value{nullptr};
  std::string path;
  bool process_lifetime{false};
  ~Handle();
};

struct LoadedSkill {
  Metadata metadata;
  const eos_skill_descriptor_v2* descriptor_v2{nullptr};
  std::unique_ptr<nlohmann::json_schema::json_validator> parameters_validator;
  std::unique_ptr<nlohmann::json_schema::json_validator> returns_validator;
  std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>>
      accepted_signal_validators;
  std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>>
      emitted_signal_validators;
  std::map<std::pair<SourceAuthority, std::string>,
           std::unique_ptr<nlohmann::json_schema::json_validator>>
      source_validators;
  std::unique_ptr<std::mutex> execution_mutex{std::make_unique<std::mutex>()};
};
}  // namespace eos::runtime::skills::native::detail

namespace eos::runtime::skills::native {
struct Runtime::Impl {
  std::vector<std::unique_ptr<detail::Handle>> handles;
  std::vector<detail::LoadedSkill> skills;
  std::map<std::pair<std::string, std::string>, detail::LoadedSkill*> skills_by_identity;
  dispatch::Dispatcher* dispatcher{nullptr};
  ResourceReader resource_reader;
  InferenceRunner inference_runner;
  detail::PrimitiveExecutor primitive_executor;
};
}  // namespace eos::runtime::skills::native

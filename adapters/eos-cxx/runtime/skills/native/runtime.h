/// Native skill loading, discovery, and synchronous execution.
#ifndef EOS_RUNTIME_SKILLS_NATIVE_RUNTIME_H_
#define EOS_RUNTIME_SKILLS_NATIVE_RUNTIME_H_

#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "adapters/eos-cxx/runtime/skills/core/provider.h"
#include "adapters/eos-cxx/runtime/dispatch/dispatcher.h"

namespace eos::runtime::dispatch {
class Dispatcher;
}

namespace eos::runtime::skills::native {
using PrimitiveReference = skills::PrimitiveReference;
using Resource = skills::Resource;
using Metadata = skills::Metadata;
using Cancelled = skills::Cancelled;
using ResourceReader = std::function<nlohmann::json(const std::string&)>;
using ResourceTemplateValidator = std::function<bool(const std::string&)>;
using InferenceRunner = std::function<InferenceResult(const InferenceRequest&, Cancelled)>;

using RunResult = skills::RunResult;
using PrimitiveRunner = std::function<dispatch::DispatchResult(
    const std::string&, const std::string&, const std::string&, nlohmann::json, Cancelled)>;
using PrimitiveCanceller = std::function<dispatch::DispatchResult(const std::string&)>;

/// Process-lifetime registry of trusted native skill libraries.
class Runtime final : public skills::Provider {
 public:
  explicit Runtime(dispatch::Dispatcher& dispatcher, ResourceReader resource_reader = {},
                   InferenceRunner inference_runner = {});
  ~Runtime();
  Runtime(Runtime&&) noexcept;
  Runtime& operator=(Runtime&&) noexcept;
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  /// Loads every `*.skill.so` in `directory` in lexical order. Primitive
  /// references are admitted against the dispatcher's exact identities. Throws
  /// std::runtime_error and admits nothing when any library is invalid.
  static Runtime LoadDirectory(const std::filesystem::path& directory,
                               dispatch::Dispatcher& dispatcher,
                               ResourceReader resource_reader = {},
                               ResourceTemplateValidator resource_template_validator = {},
                               InferenceRunner inference_runner = {});

  // Load in a worker without constructing a Dispatcher. Calls can only reach
  // hardware through the supplied broker callbacks.
  static Runtime LoadDirectoryWithBroker(
      const std::filesystem::path& directory,
      const std::vector<PrimitiveReference>& admitted_primitives,
      PrimitiveRunner primitive_runner, PrimitiveCanceller primitive_canceller,
      ResourceReader resource_reader = {},
      ResourceTemplateValidator resource_template_validator = {},
      InferenceRunner inference_runner = {});

  /// Admitted skills sorted by exact `{name, version}` identity.
  std::vector<Metadata> Discover() const override;

  /// Exact identity lookup, or nullptr when absent.
  const Metadata* Find(const std::string& name, const std::string& version) const override;

  /// Validates one argument object against an admitted skill's compiled
  /// parameters schema. Throws std::invalid_argument before execution starts.
  void ValidateArguments(const std::string& name, const std::string& version,
                         const nlohmann::json& arguments) const override;

  /// Runs skill code synchronously. Host callbacks and response slices are
  /// valid only until this function returns.
  RunResult Execute(const std::string& name, const std::string& version,
                    const nlohmann::json& arguments, const std::string& command_id,
                    Cancelled cancelled = {}) const override;

  RunResult ExecuteWithHooks(const std::string& name, const std::string& version,
                             const nlohmann::json& arguments, const std::string& command_id,
                             ExecutionHooks hooks) const override;

 private:
  struct Impl;
  static Runtime LoadDirectoryImpl(const std::filesystem::path& directory,
                                   dispatch::Dispatcher* dispatcher,
                                   const std::vector<PrimitiveReference>& admitted_primitives,
                                   PrimitiveRunner primitive_runner,
                                   PrimitiveCanceller primitive_canceller,
                                   ResourceReader resource_reader,
                                   ResourceTemplateValidator resource_template_validator,
                                   InferenceRunner inference_runner);
  explicit Runtime(std::unique_ptr<Impl> impl, dispatch::Dispatcher& dispatcher,
                    ResourceReader resource_reader, InferenceRunner inference_runner);
  explicit Runtime(std::unique_ptr<Impl> impl, PrimitiveRunner primitive_runner,
                    PrimitiveCanceller primitive_canceller,
                    ResourceReader resource_reader, InferenceRunner inference_runner);
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::runtime::skills::native

#endif  // EOS_RUNTIME_SKILLS_NATIVE_RUNTIME_H_

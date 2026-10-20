#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "src/eos/runtime/skills/core/provider.h"

namespace eos::runtime::hardware_runtime {
struct Binding;
}
namespace eos::runtime::models {
class CameraPoseStore;
class Runtime;
}

namespace eos::runtime::init {

struct Options {
  std::string config;
  std::string etc;
  std::string contracts;
  std::string skills_dir;
  std::string host;
  std::uint16_t grpc_port{0};
  std::string log_level;
  bool no_prelaunch{false};
};

using StopRequested = std::function<bool()>;

struct InferenceModelBindings {
  std::string vad;
  std::string human;
  std::string human_pose;
  std::string face;
  std::string chat;
  std::string vlm;
};

/// Resolves the optional exact chat model used by built-in skills. Missing,
/// disabled, and wrong-capability entries leave chat inference unbound.
std::string ResolveSkillChatModel(const models::Runtime& models);
std::string ResolveSkillVlmModel(const models::Runtime& models);

/// Runs one skill inference request through the canonical model contracts.
/// Vision input is either an exact retained frame or inline canonical media.
skills::InferenceResult RunSkillInference(models::Runtime& models,
                                          const models::CameraPoseStore& camera_pose,
                                          const InferenceModelBindings& bindings,
                                          const skills::InferenceRequest& inference,
                                          skills::Cancelled cancelled = {});

/// Validates every model-backed advanced primitive in a parsed binding before
/// hardware adapters are constructed.
/// Throws when its exact model is missing, disabled, or has the wrong
/// capability. Provider health is intentionally not a startup condition.
void ValidateAdvancedPrimitiveModels(const hardware_runtime::Binding& binding,
                                      const models::Runtime& models);

/// Returns false when no camera-object model is configured. Otherwise requires
/// an enabled vision.object_detection model and throws for invalid references.
bool ValidateCameraObjectModel(const models::Runtime& models, const std::string& model_id);

/// Composes the complete runtime, starts the gateway, and supervises it until
/// `stop_requested` returns true or the gateway fails.
int Run(Options options, StopRequested stop_requested);

}  // namespace eos::runtime::init

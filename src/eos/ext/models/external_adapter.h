/// Provider-specific model payload adapters kept outside the models runtime.
#pragma once

#include <memory>

#include "src/eos/runtime/models/runtime.h"

namespace eos::ext::models {

/// Creates the configured request/response adapter.
///
/// Supported names are `canonical`, `audio8-speech-v1`, `qwen3-asr-v1`, `sensevoice-vad-v1`,
/// `arcface-stream-v1`, `legacy-vision`, `rtmpose-stream-v1`, and
/// `yolo11-litter-stream-v1`. Invalid names or capability/transport combinations throw
/// std::invalid_argument during
/// runtime composition.
std::shared_ptr<const runtime::models::Adapter> CreateAdapter(
    const runtime::models::ModelConfig& model);

}  // namespace eos::ext::models

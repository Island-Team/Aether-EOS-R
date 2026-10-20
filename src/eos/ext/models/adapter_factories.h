#pragma once

#include <memory>

#include "src/eos/runtime/models/runtime.h"

namespace eos::ext::models::detail {
std::shared_ptr<const runtime::models::Adapter> CreateCanonicalAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateGemma4PostureAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateAudio8SpeechAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateQwen3AsrAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateSensevoiceVadAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateArcFaceAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateLegacyVisionAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateYolo11nRtmposeAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateYolo11LitterAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateGraspPlanAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateOpenAiChatAdapter();
std::shared_ptr<const runtime::models::Adapter> CreateOpenAiVlmAdapter();
}  // namespace eos::ext::models::detail

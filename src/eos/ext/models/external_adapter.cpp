#include "src/eos/ext/models/external_adapter.h"

#include <stdexcept>

#include "src/eos/ext/models/adapter_factories.h"
#include "src/eos/ext/models/adapter_utils.h"
#include "src/eos/lib/log.h"

namespace eos::ext::models {
using runtime::models::Capability;
using runtime::models::ModelConfig;
using runtime::models::WireFormat;

std::shared_ptr<const runtime::models::Adapter> CreateAdapter(const ModelConfig& model) {
  std::shared_ptr<const runtime::models::Adapter> adapter;
  if (model.adapter == "canonical") {
    adapter = detail::CreateCanonicalAdapter();
  }
  if (model.adapter == "gemma4-posture-v1") {
    detail::Require(model.capability == Capability::kPostureClassification, model,
                    "vision.posture_classification capability");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    detail::Require(model.transport == runtime::models::Transport::kHttp, model, "http transport");
    detail::Require(!model.provider_model_id.empty(), model, "provider_model_id");
    adapter = detail::CreateGemma4PostureAdapter();
  }
  if (model.adapter == "audio8-speech-v1") {
    detail::Require(model.capability == Capability::kTts, model, "audio.synthesis capability");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    detail::Require(!model.provider_model_id.empty(), model, "provider_model_id");
    adapter = detail::CreateAudio8SpeechAdapter();
  }
  if (model.adapter == "qwen3-asr-v1") {
    detail::Require(model.capability == Capability::kAsr, model,
                    "audio.transcription capability");
    detail::Require(model.wire_format == WireFormat::kJson, model,
                    "json wire format");
    detail::Require(!model.provider_model_id.empty(), model, "provider_model_id");
    adapter = detail::CreateQwen3AsrAdapter();
  }
  if (model.adapter == "sensevoice-vad-v1") {
    detail::Require(model.capability == Capability::kVad, model, "audio.vad capability");
    detail::Require(model.wire_format == WireFormat::kMultipart, model, "multipart wire format");
    adapter = detail::CreateSensevoiceVadAdapter();
  }
  if (model.adapter == "arcface-stream-v1") {
    detail::Require(model.capability == Capability::kFace, model, "vision.face capability");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    adapter = detail::CreateArcFaceAdapter();
  }
  if (model.adapter == "legacy-vision") {
    const bool vision = model.capability == Capability::kDetection ||
                        model.capability == Capability::kHuman ||
                        model.capability == Capability::kHumanPose ||
                        model.capability == Capability::kPose ||
                        model.capability == Capability::kPosture;
    detail::Require(vision, model, "object detection, pose, or posture capability");
    adapter = detail::CreateLegacyVisionAdapter();
  }
  if (model.adapter == "rtmpose-stream-v1") {
    const bool vision = model.capability == Capability::kHuman ||
                        model.capability == Capability::kHumanPose ||
                        model.capability == Capability::kPose ||
                        model.capability == Capability::kPosture;
    detail::Require(vision, model, "human or human-pose capability");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    adapter = detail::CreateYolo11nRtmposeAdapter();
  }
  if (model.adapter == "yolo11-litter-stream-v1") {
    detail::Require(model.capability == Capability::kDetection, model,
                    "object detection capability");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    adapter = detail::CreateYolo11LitterAdapter();
  }
  if (model.adapter == "grasp-plan-ws-v1") {
    detail::Require(model.capability == Capability::kArmGraspPlan, model, "arm.grasp_plan capability");
    detail::Require(model.transport == runtime::models::Transport::kWebSocket, model, "websocket transport");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    adapter = detail::CreateGraspPlanAdapter();
  }
  if (model.adapter == "openai-chat-v1") {
    detail::Require(model.capability == Capability::kChatCompletion, model,
                    "text.chat_completion capability");
    detail::Require(model.transport == runtime::models::Transport::kHttp, model, "http transport");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    detail::Require(!model.provider_model_id.empty(), model, "provider_model_id");
    adapter = detail::CreateOpenAiChatAdapter();
  }
  if (model.adapter == "openai-vlm-v1" || model.adapter == "gemma4-unified-v1") {
    detail::Require(model.capability == Capability::kVlm, model, "vision.vlm capability");
    detail::Require(model.transport == runtime::models::Transport::kHttp, model, "http transport");
    detail::Require(model.wire_format == WireFormat::kJson, model, "json wire format");
    detail::Require(!model.provider_model_id.empty(), model, "provider_model_id");
    adapter = detail::CreateOpenAiVlmAdapter();
  }
  if (!adapter) {
    eos::lib::LogEvent(eos::lib::GetLogger("models.adapter"), spdlog::level::warn,
                       "model_adapter_rejected",
                       {{"model_id", model.id}, {"adapter", model.adapter}, {"reason", "unknown"}});
    throw std::invalid_argument("model '" + model.id + "' has unknown adapter '" + model.adapter +
                                "'");
  }
  eos::lib::LogEvent(eos::lib::GetLogger("models.adapter"), spdlog::level::debug,
                     "model_adapter_created",
                     {{"model_id", model.id},
                      {"adapter", model.adapter},
                      {"capability", std::string(runtime::models::CapabilityName(model.capability))},
                      {"transport", model.transport == runtime::models::Transport::kWebSocket
                                        ? "websocket"
                                        : "http"},
                      {"provider_model_id", model.provider_model_id}});
  return adapter;
}
}  // namespace eos::ext::models

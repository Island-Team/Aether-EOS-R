#include "detail.h"
#include "reader.h"
#include "src/eos/runtime/models/microphone_vad.h"

namespace eos::runtime::perception::body {
using detail::ResourceUnavailable;

nlohmann::json Reader::BuildVad() {
  if (microphone_vad_ == nullptr) throw ResourceUnavailable("state projection: vad unavailable");
  const auto latest = microphone_vad_->Latest();
  if (!latest) throw ResourceUnavailable("state projection: vad unavailable");
  nlohmann::json out = StartEnvelope("vad.snapshot", "audio", "default", nullptr, "vad");
  out.erase("part_id");
  out.erase("description");
  out.erase("tags");
  out.erase("frame_id");
  out["source_sequence"] = latest->sequence;
  out["speech"] = latest->response.value("speech", false);
  out["probability"] = latest->response.value("probability", 0.0);
  if (latest->source_timestamp_ns && *latest->source_timestamp_ns >= 0)
    out["source_timestamp_ns"] = *latest->source_timestamp_ns;
  return out;
}

nlohmann::json Reader::BuildAsr() {
  if (microphone_vad_ == nullptr) throw ResourceUnavailable("state projection: asr unavailable");
  const auto latest = microphone_vad_->Latest();
  if (!latest || latest->asr_model_id.empty() || latest->asr_response.is_null()) {
    throw ResourceUnavailable("state projection: asr unavailable");
  }
  nlohmann::json out = StartEnvelope("asr.snapshot", "audio", "default", nullptr, "asr");
  out.erase("part_id");
  out.erase("description");
  out.erase("tags");
  out.erase("frame_id");
  out["schema_version"] = "1.0";
  out["text"] = latest->asr_response.value("text", latest->transcript);
  if (latest->asr_response.contains("language")) out["language"] = latest->asr_response["language"];
  if (latest->asr_response.contains("confidence"))
    out["confidence"] = latest->asr_response["confidence"];
  return out;
}

}  // namespace eos::runtime::perception::body

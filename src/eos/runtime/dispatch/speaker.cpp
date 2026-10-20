#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "dispatcher.h"
#include "speaker_audio.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::dispatch {
using namespace detail;
namespace {

constexpr std::size_t kMaximumAudioBytes = 16 * 1024 * 1024;

std::optional<std::vector<std::uint8_t>> DecodeBase64(const std::string& encoded) {
  const auto digit = [](unsigned char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  if (encoded.size() % 4 != 0) return std::nullopt;
  std::vector<std::uint8_t> decoded;
  decoded.reserve(encoded.size() / 4 * 3);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
    const bool last = offset + 4 == encoded.size();
    const unsigned char c0 = encoded[offset];
    const unsigned char c1 = encoded[offset + 1];
    const unsigned char c2 = encoded[offset + 2];
    const unsigned char c3 = encoded[offset + 3];
    const int v0 = digit(c0);
    const int v1 = digit(c1);
    const int v2 = c2 == '=' ? 0 : digit(c2);
    const int v3 = c3 == '=' ? 0 : digit(c3);
    if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0 || (!last && (c2 == '=' || c3 == '=')) ||
        (c2 == '=' && c3 != '=') || (c2 == '=' && (v1 & 0x0f) != 0) ||
        (c3 == '=' && c2 != '=' && (v2 & 0x03) != 0)) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
    if (c2 != '=') decoded.push_back(static_cast<std::uint8_t>((v1 << 4) | (v2 >> 2)));
    if (c3 != '=') decoded.push_back(static_cast<std::uint8_t>((v2 << 6) | v3));
    if (decoded.size() > kMaximumAudioBytes) return std::nullopt;
  }
  return decoded;
}

bool IsContained(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto mismatch = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
  return mismatch.first == root.end();
}

std::optional<std::vector<std::uint8_t>> ReadAsset(const std::filesystem::path& root,
                                                   const std::string& uri) {
  constexpr std::string_view kScheme = "asset://";
  if (root.empty() || uri.rfind(kScheme, 0) != 0) return std::nullopt;
  const std::string relative = uri.substr(kScheme.size());
  if (relative.empty() || relative.front() == '/' || relative.find('\\') != std::string::npos ||
      relative.find('%') != std::string::npos) {
    return std::nullopt;
  }
  const std::filesystem::path requested(relative);
  for (const auto& component : requested) {
    if (component == "." || component == "..") return std::nullopt;
  }
  std::error_code error;
  const std::filesystem::path canonical_root = std::filesystem::canonical(root, error);
  if (error) return std::nullopt;
  const std::filesystem::path file = std::filesystem::canonical(canonical_root / requested, error);
  if (error || !IsContained(canonical_root, file) ||
      !std::filesystem::is_regular_file(file, error) || error || file.extension() != ".wav") {
    return std::nullopt;
  }
  const std::uintmax_t size = std::filesystem::file_size(file, error);
  if (error || size == 0 || size > kMaximumAudioBytes) return std::nullopt;
  std::ifstream input(file, std::ios::binary);
  if (!input) return std::nullopt;
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!input || input.peek() != std::ifstream::traits_type::eof()) return std::nullopt;
  return bytes;
}

}  // namespace

nlohmann::json Dispatcher::SpeakerPlay(const std::string& primitive, const std::string& version,
                                       hardware_runtime::Device& device,
                                       const nlohmann::json& arguments) {
  const std::string uri = arguments.at("uri").get<std::string>();
  const std::optional<std::vector<std::uint8_t>> audio = ReadAsset(assets_root_, uri);
  if (!audio || !IsPcmWave(*audio)) {
    return Ack(primitive, version, arguments, false, kInvalidArgument,
               "uri must resolve to a bounded 16 kHz mono PCM WAV under the EOS asset root");
  }
  hardware::SpeakerCommand command{.asset_id = uri,
                                   .media_type = "audio/wav",
                                   .data = *audio,
                                   .stamp = std::chrono::steady_clock::now()};
  return ReleaseSpeaker(primitive, version, device, arguments, std::move(command));
}

nlohmann::json Dispatcher::SpeakerSpeak(const std::string& primitive, const std::string& version,
                                        hardware_runtime::Device& device,
                                        const nlohmann::json& arguments) {
  if (models_ == nullptr) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "TTS runtime is unavailable");
  }
  const auto declaration =
      std::find_if(device.part().primitives.begin(), device.part().primitives.end(),
                   [&](const hardware_runtime::PrimitiveDeclaration& item) {
                     return item.name == primitive && item.version == version;
                   });
  if (declaration == device.part().primitives.end() || declaration->model_id.empty()) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "speaker.speak has no admitted TTS model binding");
  }
  const models::ModelConfig* tts = models_->Find(declaration->model_id);
  if (tts == nullptr || tts->capability != models::Capability::kTts) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "bound TTS model is unavailable");
  }
  const std::optional<models::ModelHealth> health = models_->Health(tts->id);
  if (!health || health->status != models::HealthStatus::kReady) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "TTS model is not healthy");
  }
  nlohmann::json request{{"text", arguments.at("text")}, {"stream", false}};
  if (arguments.contains("language")) request["language"] = arguments.at("language");
  if (arguments.contains("voice")) request["voice"] = arguments.at("voice");
  const models::CallResult result = models_->Call(tts->id, request);
  if (result.status != models::CallResult::Status::kCompleted) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "TTS synthesis failed: " + result.message);
  }
  if (!result.response.is_object() || result.response.value("media_type", "") != "audio/wav" ||
      !result.response.contains("data_b64") || !result.response.at("data_b64").is_string()) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "TTS returned an invalid audio response");
  }
  const std::optional<std::vector<std::uint8_t>> audio =
      DecodeBase64(result.response.at("data_b64").get<std::string>());
  const std::optional<std::vector<std::uint8_t>> normalized =
      audio ? NormalizePcmWave(*audio) : std::nullopt;
  if (!normalized || !IsPcmWave(*normalized)) {
    return Ack(primitive, version, arguments, false, kUnavailableDependency,
               "TTS returned an invalid PCM WAV asset");
  }
  lib::LogEventWithAttachment(
      lib::GetLogger("dispatch.speaker"), spdlog::level::debug, "tts_audio_generated",
      {{"model_id", tts->id}, {"device_id", device.identity().device_id}}, "tts", ".wav",
      "audio/wav", *normalized);
  hardware::SpeakerCommand command{.asset_id = "tts:" + tts->id,
                                   .media_type = "audio/wav",
                                    .data = *normalized,
                                   .stamp = std::chrono::steady_clock::now()};
  return ReleaseSpeaker(primitive, version, device, arguments, std::move(command));
}

nlohmann::json Dispatcher::ReleaseSpeaker(const std::string& primitive, const std::string& version,
                                          hardware_runtime::Device& device,
                                          const nlohmann::json& arguments,
                                          hardware::SpeakerCommand command) {
  std::unique_lock<std::mutex> lock(*DeviceMutex(device.identity().device_id));
  if (registry_.EstopLatched()) {
    return Ack(primitive, version, arguments, false, kEstop,
               "estop latched; only safety.estop and motion.stop are admitted");
  }
  return Released(primitive, version, arguments, device, device.backend().ExecuteSpeaker(command),
                  primitive == "speaker.play" ? "speaker refused the audio asset"
                                              : "speaker refused synthesized audio");
}

}  // namespace eos::runtime::dispatch

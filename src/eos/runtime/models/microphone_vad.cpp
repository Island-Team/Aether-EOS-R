#include "src/eos/runtime/models/microphone_vad.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::models {
namespace {

void AppendLe16(std::vector<std::uint8_t>* bytes, std::uint16_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value));
  bytes->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendLe32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  AppendLe16(bytes, static_cast<std::uint16_t>(value));
  AppendLe16(bytes, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::uint8_t> MakeWav(const std::vector<std::int16_t>& pcm) {
  const std::size_t data_size = pcm.size() * sizeof(std::int16_t);
  if (data_size > std::numeric_limits<std::uint32_t>::max() - 36U) {
    throw std::runtime_error("microphone VAD window is too large");
  }
  std::vector<std::uint8_t> wav;
  wav.reserve(44 + data_size);
  wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
  AppendLe32(&wav, static_cast<std::uint32_t>(36 + data_size));
  wav.insert(wav.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  AppendLe32(&wav, 16);
  AppendLe16(&wav, 1);
  AppendLe16(&wav, 1);
  AppendLe32(&wav, 16000);
  AppendLe32(&wav, 32000);
  AppendLe16(&wav, 2);
  AppendLe16(&wav, 16);
  wav.insert(wav.end(), {'d', 'a', 't', 'a'});
  AppendLe32(&wav, static_cast<std::uint32_t>(data_size));
  for (const std::int16_t sample : pcm) AppendLe16(&wav, static_cast<std::uint16_t>(sample));
  return wav;
}

std::string Base64Encode(const std::vector<std::uint8_t>& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t a = input[offset];
    const std::uint32_t b = offset + 1 < input.size() ? input[offset + 1] : 0;
    const std::uint32_t c = offset + 2 < input.size() ? input[offset + 2] : 0;
    const std::uint32_t value = (a << 16) | (b << 8) | c;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[value & 0x3f] : '=');
  }
  return output;
}

void WriteWav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error("cannot create " + path.parent_path().string() + ": " +
                             error.message());
  }
  const std::vector<std::uint8_t> wav = MakeWav(pcm);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  if (!output) throw std::runtime_error("cannot write " + path.string());
}

void RemoveAudioDiagnostics(const std::filesystem::path& directory) {
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return;
  if (error)
    throw std::runtime_error("cannot inspect " + directory.string() + ": " + error.message());
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_regular_file() && entry.path().extension() == ".wav" &&
        (name.rfind("vad-", 0) == 0 || name.rfind("asr-", 0) == 0)) {
      std::filesystem::remove(entry.path(), error);
      if (error) {
        throw std::runtime_error("cannot remove " + entry.path().string() + ": " + error.message());
      }
    }
  }
}

}  // namespace

void MicrophoneVadStore::Set(Snapshot snapshot, bool asr_updated) {
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.sequence = ++sequence_;
    latest_ = std::move(snapshot);
    updated = updated_;
  }
  if (updated) updated(asr_updated);
}

void MicrophoneVadStore::SetUpdatedCallback(Updated updated) {
  std::lock_guard<std::mutex> lock(mutex_);
  updated_ = std::move(updated);
}

std::optional<MicrophoneVadStore::Snapshot> MicrophoneVadStore::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

MicrophoneVadPump::MicrophoneVadPump(Runtime& models, Capture capture, MicrophoneVadStore& store,
                                     std::string model_id, std::chrono::milliseconds window,
                                     std::chrono::milliseconds poll_interval,
                                     std::string asr_model_id,
                                     std::filesystem::path diagnostics_directory,
                                     bool overwrite_diagnostics)
    : models_(models),
      capture_(std::move(capture)),
      store_(store),
      model_id_(std::move(model_id)),
      asr_model_id_(std::move(asr_model_id)),
      diagnostics_directory_(std::move(diagnostics_directory)),
      overwrite_diagnostics_(overwrite_diagnostics),
      window_(window),
      poll_interval_(poll_interval) {
  if (!capture_) throw std::invalid_argument("microphone VAD capture callback is required");
  if (model_id_.empty()) throw std::invalid_argument("microphone VAD model id is required");
  if (window_ <= std::chrono::milliseconds::zero() ||
      poll_interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("microphone VAD intervals must be positive");
  }
  worker_ = std::thread(&MicrophoneVadPump::Run, this);
}

MicrophoneVadPump::~MicrophoneVadPump() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wakeup_.notify_all();
  worker_.join();
}

void MicrophoneVadPump::Run() {
  auto logger = eos::lib::GetLogger("models.microphone_vad");
  if (overwrite_diagnostics_ && !diagnostics_directory_.empty()) {
    try {
      RemoveAudioDiagnostics(diagnostics_directory_);
    } catch (const std::exception& error) {
      eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_audio_overwrite_failed",
                         {{"path", diagnostics_directory_.string()}, {"reason", error.what()}});
    }
  }
  eos::lib::LogEvent(logger, spdlog::level::info, "microphone_vad_started",
                      {{"model_id", model_id_},
                       {"asr_model_id", asr_model_id_},
                       {"window_ms", window_.count()},
                      {"poll_interval_ms", poll_interval_.count()}});
  std::vector<std::int16_t> pcm;
  std::vector<std::int16_t> pre_roll;
  std::vector<std::int16_t> utterance;
  std::string transcript;
  std::string successful_asr_model_id;
  nlohmann::json asr_response;
  const auto session = std::chrono::system_clock::now().time_since_epoch().count();
  std::size_t utterance_sequence = 0;
  std::optional<std::int64_t> last_timestamp;
  std::optional<bool> last_speech;
  std::size_t consecutive_failures = 0;
  std::size_t backpressure_events = 0;
  std::int64_t last_vad_call_ms = 0;
  std::int64_t last_asr_call_ms = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    lock.unlock();
    try {
      const auto block = capture_();
      if (block && block->sample_rate_hz == 16000 && block->channels == 1 && !block->pcm.empty() &&
          (!block->source_timestamp_ns || block->source_timestamp_ns != last_timestamp)) {
        last_timestamp = block->source_timestamp_ns;
        pcm.insert(pcm.end(), block->pcm.begin(), block->pcm.end());
        const std::size_t wanted = static_cast<std::size_t>(window_.count()) * 16U;
        const std::size_t max_pending = wanted * 2U;
        if (pcm.size() > max_pending) {
          const std::size_t dropped = pcm.size() - max_pending;
          pcm.erase(pcm.begin(), pcm.begin() + static_cast<std::ptrdiff_t>(dropped));
          ++backpressure_events;
          if ((backpressure_events & (backpressure_events - 1U)) == 0) {
            eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_audio_dropped",
                               {{"sample_count", dropped},
                                {"lost_duration_ms", dropped / 16U},
                                {"pending_duration_ms", pcm.size() / 16U},
                                {"events", backpressure_events},
                                {"model_id", model_id_},
                                {"last_vad_call_ms", last_vad_call_ms},
                                {"last_asr_call_ms", last_asr_call_ms},
                                {"reason", "vad_backpressure"}},
                               "Microphone audio was dropped while VAD was busy");
          }
        }
        if (pcm.size() >= wanted) {
          std::vector<std::int16_t> window(pcm.begin(), pcm.begin() + wanted);
          pcm.erase(pcm.begin(), pcm.begin() + wanted);
          const nlohmann::json request = {{"input",
                                           {{"data_b64", Base64Encode(MakeWav(window))},
                                            {"filename", "microphone.wav"},
                                            {"media_type", "audio/wav"},
                                            {"sample_rate_hz", 16000},
                                            {"channels", 1}}}};
          const auto vad_started = std::chrono::steady_clock::now();
          CallResult result = models_.Call(model_id_, request, [this] {
            std::lock_guard<std::mutex> lock(mutex_);
            return stopping_;
          });
          last_vad_call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - vad_started)
                                 .count();
          if (result.status == CallResult::Status::kCompleted) {
            bool asr_updated = false;
            if (consecutive_failures != 0) {
              eos::lib::LogEvent(logger, spdlog::level::info, "microphone_vad_recovered",
                                 {{"model_id", model_id_},
                                  {"failures", consecutive_failures}});
              consecutive_failures = 0;
            }
            const bool speech = result.response.value("speech", false);
            if (!last_speech || *last_speech != speech) {
              eos::lib::LogEvent(logger, spdlog::level::info, "microphone_vad_state_changed",
                                 {{"model_id", model_id_},
                                  {"speech", speech},
                                  {"probability", result.response.value("probability", 0.0)}});
              last_speech = speech;
            }
            if (speech) {
              if (utterance.empty()) {
                utterance.insert(utterance.end(), pre_roll.begin(), pre_roll.end());
              }
              utterance.insert(utterance.end(), window.begin(), window.end());
              constexpr std::size_t kMaxUtteranceSamples = 16000U * 30U;
              if (utterance.size() > kMaxUtteranceSamples) {
                eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_utterance_dropped",
                                   {{"sample_count", utterance.size()},
                                    {"reason", "duration_limit"}});
                utterance.clear();
                pre_roll.clear();
              }
            } else if (!utterance.empty()) {
              utterance.insert(utterance.end(), window.begin(), window.end());
              const std::vector<std::uint8_t> utterance_wav = MakeWav(utterance);
              eos::lib::LogEventWithAttachment(
                  logger, spdlog::level::debug, "microphone_asr_requested",
                  {{"model_id", asr_model_id_}, {"sample_count", utterance.size()}}, "asr",
                  ".wav", "audio/wav", utterance_wav);
              if (!diagnostics_directory_.empty()) {
                std::ostringstream filename;
                filename << "asr-" << session << '-' << std::setw(6) << std::setfill('0')
                         << ++utterance_sequence << ".wav";
                const std::filesystem::path path = diagnostics_directory_ / filename.str();
                try {
                  WriteWav(path, utterance);
                } catch (const std::exception& error) {
                  eos::lib::LogEvent(logger, spdlog::level::warn,
                                     "microphone_asr_audio_write_failed",
                                     {{"path", path.string()}, {"reason", error.what()}});
                }
              }
              const nlohmann::json asr_request = {{"input",
                                                   {{"data_b64", Base64Encode(utterance_wav)},
                                                    {"filename", "utterance.wav"},
                                                    {"media_type", "audio/wav"}}}};
              if (!asr_model_id_.empty()) {
                const auto asr_started = std::chrono::steady_clock::now();
                CallResult asr = models_.Call(asr_model_id_, asr_request, [this] {
                  std::lock_guard<std::mutex> lock(mutex_);
                  return stopping_;
                });
                last_asr_call_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - asr_started)
                                       .count();
                if (asr.status == CallResult::Status::kCompleted) {
                  transcript = asr.response.value("text", "");
                  successful_asr_model_id = asr_model_id_;
                  asr_response = std::move(asr.response);
                  asr_updated = true;
                  eos::lib::LogEvent(logger, spdlog::level::info, "microphone_asr_completed",
                                     {{"model_id", asr_model_id_},
                                      {"sample_count", utterance.size()}});
                } else {
                  eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_asr_failed",
                                     {{"model_id", asr_model_id_}, {"reason", asr.message}});
                }
              }
              utterance.clear();
            }
            if (!speech) pre_roll = window;
            store_.Set({model_id_, last_timestamp, std::move(result.response), transcript,
                         successful_asr_model_id, asr_response}, asr_updated);
          } else {
            ++consecutive_failures;
            pcm.clear();
            pre_roll.clear();
            utterance.clear();
            if ((consecutive_failures & (consecutive_failures - 1U)) == 0) {
              eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_vad_failed",
                                 {{"model_id", model_id_},
                                  {"consecutive_failures", consecutive_failures},
                                  {"reason", result.message}});
            }
          }
        }
      }
    } catch (const std::exception& error) {
      pcm.clear();
      pre_roll.clear();
      utterance.clear();
      ++consecutive_failures;
      if ((consecutive_failures & (consecutive_failures - 1U)) == 0) {
        eos::lib::LogEvent(logger, spdlog::level::warn, "microphone_vad_failed",
                           {{"model_id", model_id_},
                            {"consecutive_failures", consecutive_failures},
                            {"reason", error.what()}});
      }
    }
    lock.lock();
    wakeup_.wait_for(lock, poll_interval_, [this] { return stopping_; });
  }
  lock.unlock();
  if (!utterance.empty()) {
    eos::lib::LogEvent(logger, spdlog::level::info, "microphone_utterance_discarded",
                       {{"sample_count", utterance.size()}, {"reason", "shutdown"}});
  }
  eos::lib::LogEvent(logger, spdlog::level::info, "microphone_vad_stopped",
                     {{"model_id", model_id_}});
}

}  // namespace eos::runtime::models

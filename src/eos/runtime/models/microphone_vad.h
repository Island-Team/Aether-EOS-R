/// Accumulates microphone PCM windows and submits canonical WAV audio to VAD.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

#include "eos/hardware/backend.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::models {

class MicrophoneVadStore final {
 public:
  struct Snapshot {
    std::string model_id;
    std::optional<std::int64_t> source_timestamp_ns;
    nlohmann::json response;
    std::string transcript;
    std::string asr_model_id;
    nlohmann::json asr_response;
    std::uint64_t sequence{0};
  };

  using Updated = std::function<void(bool asr_updated)>;

  void Set(Snapshot snapshot, bool asr_updated = false);
  std::optional<Snapshot> Latest() const;
  void SetUpdatedCallback(Updated updated);

 private:
  mutable std::mutex mutex_;
  std::optional<Snapshot> latest_;
  Updated updated_;
  std::uint64_t sequence_{0};
};

class MicrophoneVadPump final {
 public:
  using Capture = std::function<std::optional<hardware::MicrophoneTelemetry>()>;

  MicrophoneVadPump(Runtime& models, Capture capture, MicrophoneVadStore& store,
                    std::string model_id, std::chrono::milliseconds window,
                    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10),
                    std::string asr_model_id = "asr",
                    std::filesystem::path diagnostics_directory = {},
                    bool overwrite_diagnostics = false);
  ~MicrophoneVadPump();
  MicrophoneVadPump(const MicrophoneVadPump&) = delete;
  MicrophoneVadPump& operator=(const MicrophoneVadPump&) = delete;

 private:
  void Run();

  Runtime& models_;
  Capture capture_;
  MicrophoneVadStore& store_;
  std::string model_id_;
  std::string asr_model_id_;
  std::filesystem::path diagnostics_directory_;
  bool overwrite_diagnostics_;
  std::chrono::milliseconds window_;
  std::chrono::milliseconds poll_interval_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  bool stopping_{false};
  std::thread worker_;
};

}  // namespace eos::runtime::models

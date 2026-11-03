#include "aplay_speaker.h"

#include <fcntl.h>
#include <linux/memfd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "src/eos/lib/log.h"

extern char** environ;

namespace eos::backends::aplay {
namespace {

constexpr auto kMaximumPlaybackDuration = std::chrono::minutes(5);
constexpr auto kPlaybackExitMargin = std::chrono::seconds(5);

std::uint16_t ReadLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1] << 8);
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

bool HasTag(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char tag[5]) {
  return offset + 4 <= bytes.size() && std::memcmp(bytes.data() + offset, tag, 4) == 0;
}

bool ValidatePcmWave(const std::vector<std::uint8_t>& bytes, std::chrono::milliseconds* duration) {
  if (bytes.size() < 44 || !HasTag(bytes, 0, "RIFF") || !HasTag(bytes, 8, "WAVE") ||
      ReadLe32(bytes, 4) != bytes.size() - 8) {
    return false;
  }
  bool saw_format = false;
  bool saw_data = false;
  std::uint16_t block_align = 0;
  std::uint32_t byte_rate = 0;
  std::size_t data_size = 0;
  std::size_t offset = 12;
  while (offset < bytes.size()) {
    if (offset + 8 > bytes.size()) return false;
    const std::uint32_t chunk_size = ReadLe32(bytes, offset + 4);
    const std::size_t payload = offset + 8;
    if (chunk_size > bytes.size() - payload) return false;
    const std::size_t end = payload + chunk_size;
    if (HasTag(bytes, offset, "fmt ")) {
      if (saw_format || chunk_size < 16) return false;
      const std::uint16_t format = ReadLe16(bytes, payload);
      const std::uint16_t channels = ReadLe16(bytes, payload + 2);
      const std::uint32_t sample_rate = ReadLe32(bytes, payload + 4);
      byte_rate = ReadLe32(bytes, payload + 8);
      block_align = ReadLe16(bytes, payload + 12);
      const std::uint16_t bits_per_sample = ReadLe16(bytes, payload + 14);
      const std::uint32_t expected_align =
          static_cast<std::uint32_t>(channels) * bits_per_sample / 8;
      const std::uint64_t expected_rate = static_cast<std::uint64_t>(sample_rate) * expected_align;
      if (format != 1 || channels == 0 || sample_rate == 0 || bits_per_sample == 0 ||
          bits_per_sample % 8 != 0 || expected_align == 0 || expected_align > 0xffff ||
          block_align != expected_align || expected_rate > UINT32_MAX ||
          byte_rate != expected_rate) {
        return false;
      }
      saw_format = true;
    } else if (HasTag(bytes, offset, "data")) {
      if (!saw_format || saw_data || chunk_size == 0) return false;
      data_size = chunk_size;
      saw_data = true;
    }
    offset = end + (chunk_size & 1U);
    if (offset > bytes.size()) return false;
  }
  if (offset != bytes.size() || !saw_format || !saw_data || block_align == 0 || byte_rate == 0 ||
      data_size % block_align != 0) {
    return false;
  }
  const std::uint64_t milliseconds =
      (static_cast<std::uint64_t>(data_size) * 1000 + byte_rate - 1) / byte_rate;
  if (milliseconds == 0 || milliseconds > static_cast<std::uint64_t>(
                                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  kMaximumPlaybackDuration)
                                                  .count())) {
    return false;
  }
  *duration = std::chrono::milliseconds(milliseconds);
  return true;
}

bool WriteAll(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

PlaybackOutcome Failure(const std::string& stage, int error) {
  return {false, stage + " failed: " + std::string(std::strerror(error))};
}

std::string PlaybackDiagnostic(int fd) {
  if (lseek(fd, 0, SEEK_SET) < 0) return {};
  char buffer[4097];
  const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
  if (length <= 0) return {};
  std::string result(buffer, static_cast<std::size_t>(length));
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
  return result;
}

PlaybackOutcome PlayWithAplay(const std::vector<std::uint8_t>& audio, const std::string& device,
                              std::chrono::milliseconds duration,
                              const CancellationProbe& cancelled) {
  if (cancelled()) return {false, "playback was cancelled"};
  int input = static_cast<int>(syscall(SYS_memfd_create, "eos-speaker", MFD_CLOEXEC));
  if (input < 0) return Failure("creating the staging buffer", errno);
  if (input <= STDERR_FILENO) {
    const int duplicate = fcntl(input, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    const int duplicate_error = errno;
    close(input);
    input = duplicate;
    if (input < 0) return Failure("duplicating the audio descriptor", duplicate_error);
  }
  if (!WriteAll(input, audio)) {
    const int write_error = errno;
    close(input);
    return Failure("staging the audio buffer", write_error);
  }
  if (lseek(input, 0, SEEK_SET) < 0) {
    const int seek_error = errno;
    close(input);
    return Failure("rewinding the audio buffer", seek_error);
  }

  const int diagnostic = static_cast<int>(syscall(SYS_memfd_create, "eos-aplay-stderr", MFD_CLOEXEC));
  if (diagnostic < 0) {
    const int error = errno;
    close(input);
    return Failure("creating the playback diagnostic buffer", error);
  }

  posix_spawn_file_actions_t actions;
  const int actions_error = posix_spawn_file_actions_init(&actions);
  if (actions_error != 0) {
    close(input);
    close(diagnostic);
    return Failure("preparing the aplay spawn", actions_error);
  }
  bool actions_ok = posix_spawn_file_actions_adddup2(&actions, input, STDIN_FILENO) == 0 &&
                    posix_spawn_file_actions_addclose(&actions, input) == 0 &&
                    posix_spawn_file_actions_adddup2(&actions, diagnostic, STDERR_FILENO) == 0 &&
                    posix_spawn_file_actions_addclose(&actions, diagnostic) == 0;
  pid_t child = -1;
  std::string program = "/usr/bin/aplay";
  std::string quiet = "-q";
  std::string device_option = "-D";
  char* arguments[] = {program.data(), quiet.data(), device_option.data(),
                       const_cast<char*>(device.c_str()), nullptr};
  const int spawn_error =
      actions_ok ? posix_spawn(&child, program.c_str(), &actions, nullptr, arguments, environ)
                 : EINVAL;
  posix_spawn_file_actions_destroy(&actions);
  close(input);
  if (spawn_error != 0) {
    close(diagnostic);
    return Failure("launching aplay", spawn_error);
  }

  bool terminating = false;
  bool killed = false;
  bool cancelled_during_playback = false;
  auto kill_deadline = std::chrono::steady_clock::time_point{};
  const auto playback_deadline = std::chrono::steady_clock::now() + duration + kPlaybackExitMargin;
  int status = 0;
  for (;;) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) {
      const int error = errno;
      close(diagnostic);
      return Failure("waiting for aplay", error);
    }
    const bool cancelled_now = cancelled();
    if (cancelled_now || std::chrono::steady_clock::now() >= playback_deadline) {
      if (!terminating) {
        cancelled_during_playback = cancelled_now;
        kill(child, SIGTERM);
        terminating = true;
        kill_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      } else if (!killed && std::chrono::steady_clock::now() >= kill_deadline) {
        kill(child, SIGKILL);
        killed = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const std::string diagnostic_text = PlaybackDiagnostic(diagnostic);
  close(diagnostic);
  if (terminating) {
    return {false, cancelled_during_playback ? "playback was cancelled"
                                             : "playback exceeded its deadline"};
  }
  if (!WIFEXITED(status)) {
    return {false, "aplay was terminated by signal " + std::to_string(WTERMSIG(status)) +
                       (diagnostic_text.empty() ? "" : ": " + diagnostic_text)};
  }
  if (WEXITSTATUS(status) != 0) {
    return {false, "aplay exited with status " + std::to_string(WEXITSTATUS(status)) +
                       (diagnostic_text.empty() ? "" : ": " + diagnostic_text)};
  }
  return {true, {}};
}

}  // namespace

struct AplaySpeaker::Impl {
  struct State {
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    bool stopping{false};
    bool halted{false};
    bool playback_active{false};
    bool audio_ready{false};
    std::uint64_t generation{0};
    std::string active_asset_id;
    std::optional<std::string> last_asset_id;
    std::string media_type;
    std::vector<std::uint8_t> audio;
    std::chrono::milliseconds duration{0};
    hardware::SpeakerPlaybackState playback_state{hardware::SpeakerPlaybackState::kIdle};
    std::optional<std::uint64_t> played_duration_ms;
    std::string last_failure_reason;
    std::uint64_t underruns{0};
  };

  AplaySpeakerConfig config;
  Playback playback;
  std::shared_ptr<State> state{std::make_shared<State>()};
  std::thread worker;

  Impl(AplaySpeakerConfig supplied, Playback player)
      : config(std::move(supplied)),
        playback(player ? std::move(player) : Playback(PlayWithAplay)) {
    if (config.device_id.empty() || config.alsa_device.empty() || config.max_stream_bytes == 0 ||
        !playback) {
      throw std::invalid_argument("invalid aplay speaker configuration");
    }
    worker = std::thread([this, state = state] { Run(std::move(state)); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stopping = true;
      ++state->generation;
      state->audio_ready = false;
      state->audio.clear();
      state->playback_state = hardware::SpeakerPlaybackState::kIdle;
    }
    state->wakeup.notify_all();
    if (worker.joinable()) worker.join();
  }

  void Run(std::shared_ptr<State> shared) {
    for (;;) {
      std::vector<std::uint8_t> audio;
      std::chrono::milliseconds duration{0};
      std::uint64_t token = 0;
      {
        std::unique_lock<std::mutex> lock(shared->mutex);
        shared->wakeup.wait(lock, [&] { return shared->stopping || shared->audio_ready; });
        if (shared->stopping) return;
        audio = std::move(shared->audio);
        duration = shared->duration;
        token = shared->generation;
        shared->audio_ready = false;
        shared->playback_active = true;
        shared->playback_state = hardware::SpeakerPlaybackState::kPlaying;
      }

      PlaybackOutcome outcome;
      try {
        const std::weak_ptr<State> weak = shared;
        const auto cancelled = [weak, token] {
          const auto current = weak.lock();
          if (!current) return true;
          std::lock_guard<std::mutex> lock(current->mutex);
          return current->stopping || current->generation != token;
        };
        outcome = playback(audio, config.alsa_device, duration, cancelled);
        if (!outcome.ok && !cancelled()) {
          eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::err,
                             "speaker_playback_failed",
                             {{"device", config.alsa_device},
                              {"duration_ms", duration.count()},
                              {"reason", outcome.reason}});
        } else if (outcome.ok) {
          eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::debug,
                             "speaker_playback_completed",
                             {{"device", config.alsa_device}, {"duration_ms", duration.count()}});
        }
      } catch (const std::exception& error) {
        outcome = {false, error.what()};
        eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::err,
                           "speaker_playback_exception",
                           {{"device", config.alsa_device}, {"reason", error.what()}});
      } catch (...) {
        outcome = {false, "unknown"};
        eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::err,
                           "speaker_playback_exception",
                           {{"device", config.alsa_device}, {"reason", "unknown"}});
      }

      {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->playback_active = false;
        if (shared->generation == token) {
          shared->playback_state = outcome.ok ? hardware::SpeakerPlaybackState::kIdle
                                              : hardware::SpeakerPlaybackState::kFault;
          // Retained while the fault is observable; ReleaseFailureReason serves
          // it and the next accepted command clears it.
          shared->last_failure_reason =
              outcome.ok ? std::string{}
                         : (outcome.reason.empty() ? "speaker playback failed" : outcome.reason);
          shared->played_duration_ms =
              outcome.ok ? std::optional<std::uint64_t>(duration.count()) : std::nullopt;
          shared->active_asset_id.clear();
          shared->media_type.clear();
        }
      }
      shared->wakeup.notify_all();
    }
  }

  void Cancel(bool latch_halt) {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (latch_halt && state->halted) return;
      if (latch_halt) state->halted = true;
      ++state->generation;
      state->audio_ready = false;
      state->audio.clear();
      state->active_asset_id.clear();
      state->media_type.clear();
      state->playback_state = hardware::SpeakerPlaybackState::kIdle;
    }
    state->wakeup.notify_all();
  }
};

AplaySpeaker::AplaySpeaker(AplaySpeakerConfig config, Playback playback)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(playback))) {
  eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::info, "speaker_backend_started",
                     {{"device", impl_->config.device_id}});
}

AplaySpeaker::~AplaySpeaker() {
  eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::info, "speaker_backend_stopped",
                     {{"device", impl_->config.device_id}});
}

hardware::BackendIdentity AplaySpeaker::Identify() const {
  return {.name = "aplay-speaker",
          .version = "1.0.0",
          .device_id = impl_->config.device_id,
          .kind = hardware::DeviceKind::kSpeaker};
}

hardware::FeedDeclarations AplaySpeaker::DeclareFeeds() const {
  return {.geometric = {.max_rate_hz = 0.0},
          .proprioception = {.max_rate_hz = 10.0},
          .sensor = {.max_rate_hz = 0.0},
          .health = {.max_rate_hz = 10.0}};
}

hardware::ReleaseResult AplaySpeaker::ExecuteSpeaker(const hardware::SpeakerCommand& command) {
  std::chrono::milliseconds duration{0};
  if (command.asset_id.empty() || command.asset_id.size() > 2048 ||
      command.media_type != "audio/wav" || command.data.empty() ||
      command.data.size() > impl_->config.max_stream_bytes ||
      !ValidatePcmWave(command.data, &duration)) {
    eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::warn,
                       "speaker_command_rejected",
                       {{"reason", "invalid_asset"},
                        {"asset_id", command.asset_id},
                        {"media_type", command.media_type},
                        {"asset_bytes", command.data.size()}});
    return hardware::ReleaseResult::kRejected;
  }
  {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    if (impl_->state->stopping || impl_->state->halted || impl_->state->playback_active ||
        impl_->state->audio_ready) {
      eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::warn,
                         "speaker_command_rejected", {{"reason", "backend_unavailable"}});
      return hardware::ReleaseResult::kRejected;
    }
    ++impl_->state->generation;
    impl_->state->active_asset_id = command.asset_id;
    impl_->state->last_asset_id = command.asset_id;
    impl_->state->media_type = command.media_type;
    impl_->state->played_duration_ms.reset();
    impl_->state->last_failure_reason.clear();
    impl_->state->audio = command.data;
    impl_->state->duration = duration;
    impl_->state->audio_ready = true;
    impl_->state->playback_state = hardware::SpeakerPlaybackState::kDraining;
  }
  impl_->state->wakeup.notify_one();
  eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::debug,
                     "speaker_stream_accepted",
                     {{"asset_id", command.asset_id}, {"bytes", command.data.size()}});
  return hardware::ReleaseResult::kAccepted;
}

std::optional<hardware::SpeakerTelemetry> AplaySpeaker::ReadSpeakerTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  const auto queued = impl_->state->playback_state == hardware::SpeakerPlaybackState::kBuffering
                          ? std::optional<std::uint64_t>{}
                      : impl_->state->playback_state == hardware::SpeakerPlaybackState::kIdle
                          ? std::optional<std::uint64_t>(0)
                          : std::optional<std::uint64_t>(impl_->state->duration.count());
  return hardware::SpeakerTelemetry{
      .state = impl_->state->playback_state,
      .asset_id = impl_->state->active_asset_id.empty()
                      ? impl_->state->last_asset_id
                      : std::optional<std::string>(impl_->state->active_asset_id),
      .queued_duration_ms = queued,
      .played_duration_ms = impl_->state->played_duration_ms,
      .underruns = impl_->state->underruns,
      .source_timestamp_ns = std::nullopt,
  };
}

hardware::BackendHealth AplaySpeaker::ReadHealth() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return {.alive = !impl_->state->halted, .admitted = IsAdmitted()};
}

std::string AplaySpeaker::ReleaseFailureReason() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  // Deliberately distinct from the base default, which callers treat as
  // "no specific cause" and replace with their own fallback text.
  return impl_->state->last_failure_reason.empty() ? "aplay speaker refused the release"
                                                  : impl_->state->last_failure_reason;
}

void AplaySpeaker::Clamp() { impl_->Cancel(false); }

void AplaySpeaker::Halt() {
  impl_->Cancel(true);
  {
    std::unique_lock<std::mutex> lock(impl_->state->mutex);
    impl_->state->wakeup.wait_for(lock, std::chrono::seconds(2),
                                  [&] { return !impl_->state->playback_active; });
  }
  eos::lib::LogEvent(eos::lib::GetLogger("speaker"), spdlog::level::warn, "speaker_backend_halted",
                     {{"device", impl_->config.device_id}});
}

}  // namespace eos::backends::aplay

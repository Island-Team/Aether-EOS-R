#include "alsa_mic.h"

#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "src/eos/lib/log.h"

extern char** environ;

namespace eos::backends::alsa {
namespace {

constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr std::size_t kMaxBufferedSeconds = 60;

/// Reads exactly `bytes` into `out`, honouring a bounded wait. Returns false on
/// end of stream, error, or timeout.
bool ReadExact(int fd, std::size_t bytes, int timeout_ms, std::vector<std::uint8_t>* out) {
  out->clear();
  out->reserve(bytes);
  while (out->size() < bytes) {
    pollfd descriptor{};
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    const int ready = ::poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) return false;
    std::uint8_t buffer[4096];
    const std::size_t wanted = std::min(sizeof(buffer), bytes - out->size());
    const ssize_t read_bytes = ::read(fd, buffer, wanted);
    if (read_bytes <= 0) return false;
    out->insert(out->end(), buffer, buffer + read_bytes);
  }
  return true;
}

/// Spawns `arecord` in raw mode and reads its stdout as the capture stream.
///
/// Spawning still succeeds when the device is absent: arecord then exits and
/// the first read sees end of stream, which the caller reports as absence.
bool OpenArecord(const AlsaMicConfig& config, CaptureStream* stream) {
  const std::vector<std::string> argv = {"arecord", "-q",
                                         "-D",      config.alsa_device,
                                         "-t",      "raw",
                                         "-f",      "S16_LE",
                                         "-r",      std::to_string(config.sample_rate_hz),
                                         "-c",      std::to_string(config.channels),
                                         "-"};
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& argument : argv) {
    raw.push_back(const_cast<char*>(argument.c_str()));
  }
  raw.push_back(nullptr);

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return false;

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return false;
  }
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);

  pid_t child = 0;
  const int spawned = ::posix_spawnp(&child, raw[0], &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);
  if (spawned != 0) {
    ::close(fds[0]);
    return false;
  }

  // The child outlives this call, so the descriptor and pid travel with the
  // returned stream rather than with a stack frame.
  auto descriptor = std::make_shared<int>(fds[0]);
  auto pid = std::make_shared<pid_t>(child);
  const int timeout_ms = static_cast<int>(config.read_timeout_ms);
  const std::size_t window_bytes =
      static_cast<std::size_t>(config.window_frames) * config.channels * sizeof(std::int16_t);

  stream->read = [descriptor, timeout_ms, window_bytes](std::vector<std::int16_t>* frames,
                                                        std::size_t /*frames_wanted*/) {
    const std::size_t bytes = window_bytes;
    std::vector<std::uint8_t> raw_bytes;
    if (!ReadExact(*descriptor, bytes, timeout_ms, &raw_bytes)) return false;
    frames->clear();
    frames->reserve(bytes / sizeof(std::int16_t));
    for (std::size_t offset = 0; offset + 1 < raw_bytes.size(); offset += 2) {
      frames->push_back(
          static_cast<std::int16_t>(static_cast<std::uint16_t>(raw_bytes[offset]) |
                                    (static_cast<std::uint16_t>(raw_bytes[offset + 1]) << 8)));
    }
    return !frames->empty();
  };
  stream->stop = [descriptor, pid] {
    if (*descriptor >= 0) {
      ::close(*descriptor);
      *descriptor = -1;
    }
    if (*pid > 0) {
      ::kill(*pid, SIGTERM);
      int status = 0;
      ::waitpid(*pid, &status, 0);
      *pid = -1;
    }
  };
  return true;
}

}  // namespace

struct AlsaMic::Impl {
  struct State {
    std::mutex mutex;
    std::condition_variable wakeup;
    bool stopping{false};
    /// True once a window has actually been delivered, so a spawned-but-dead
    /// device never reads as alive.
    bool delivering{false};
  };

  AlsaMicConfig config;
  CaptureOpen open;
  std::shared_ptr<State> state{std::make_shared<State>()};
  mutable std::mutex window_mutex;
  mutable std::vector<std::int16_t> window;
  mutable std::optional<std::int64_t> source_timestamp_ns;
  bool halted{false};
  std::thread worker;

  Impl(AlsaMicConfig supplied, CaptureOpen supplied_open)
      : config(std::move(supplied)),
        open(supplied_open ? std::move(supplied_open) : CaptureOpen(OpenArecord)) {
    if (config.device_id.empty() || config.alsa_device.empty() || config.frame_id.empty() ||
        config.sample_rate_hz == 0 || config.channels == 0 || config.window_frames == 0 ||
        config.read_timeout_ms == 0 || !open) {
      throw std::invalid_argument("invalid alsa microphone configuration");
    }
    worker = std::thread([this, state = state] { Run(std::move(state)); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->stopping = true;
    }
    state->wakeup.notify_all();
    if (worker.joinable()) worker.join();
  }

  bool Halted() const {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->stopping;
  }

  void Publish(const std::vector<std::int16_t>& frames) {
    std::lock_guard<std::mutex> lock(window_mutex);
    window.insert(window.end(), frames.begin(), frames.end());
    const std::size_t maximum_samples =
        static_cast<std::size_t>(config.sample_rate_hz) * config.channels * kMaxBufferedSeconds;
    if (window.size() > maximum_samples) {
      window.erase(window.begin(), window.begin() + (window.size() - maximum_samples));
    }
    source_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(window_mutex);
    window.clear();
    source_timestamp_ns.reset();
  }

  void Run(std::shared_ptr<State> shared) {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (shared->stopping) return;
      }

      CaptureStream stream;
      if (open(config, &stream) && stream.read && stream.stop) {
        bool delivered = false;
        const auto cancelled = [shared] {
          std::lock_guard<std::mutex> lock(shared->mutex);
          return shared->stopping;
        };
        for (;;) {
          if (cancelled()) break;
          std::vector<std::int16_t> frames;
          if (!stream.read(&frames, config.window_frames)) break;
          Publish(frames);
          {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->delivering = true;
          }
          if (!delivered) {
            delivered = true;
            eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::info,
                               "microphone_capture_started",
                               {{"device", config.device_id},
                                {"sample_rate_hz", config.sample_rate_hz},
                                {"channels", config.channels},
                                {"window_frames", config.window_frames}});
          }
        }
        stream.stop();
        Clear();
        {
          std::lock_guard<std::mutex> lock(shared->mutex);
          shared->delivering = false;
        }
        if (!cancelled()) {
          eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::warn,
                             "microphone_capture_interrupted",
                             {{"device", config.device_id},
                              {"delivered", delivered},
                              {"retry_ms", kRetryInterval.count() * 1000}});
        }
      } else {
        eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::warn,
                           "microphone_open_failed",
                           {{"device", config.device_id},
                            {"alsa_device", config.alsa_device},
                            {"retry_ms", kRetryInterval.count() * 1000}});
      }

      std::unique_lock<std::mutex> lock(shared->mutex);
      if (shared->stopping) return;
      // Wait before retrying so a device attached later is picked up without
      // spinning, and so shutdown stays prompt.
      shared->wakeup.wait_for(lock, kRetryInterval, [&] { return shared->stopping; });
    }
  }
};

AlsaMic::AlsaMic(AlsaMicConfig config, CaptureOpen open)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(open))) {
  eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::info,
                     "microphone_backend_started",
                     {{"device", impl_->config.device_id},
                      {"alsa_device", impl_->config.alsa_device},
                      {"sample_rate_hz", impl_->config.sample_rate_hz},
                      {"channels", impl_->config.channels},
                      {"window_frames", impl_->config.window_frames}});
}

AlsaMic::~AlsaMic() {
  const std::string device = impl_->config.device_id;
  impl_.reset();
  eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::info,
                     "microphone_backend_stopped", {{"device", device}});
}

hardware::BackendIdentity AlsaMic::Identify() const {
  return {.name = "alsa-mic",
          .version = "1.0.0",
          .device_id = impl_->config.device_id,
          .kind = hardware::DeviceKind::kMicrophone};
}

hardware::FeedDeclarations AlsaMic::DeclareFeeds() const {
  return {.geometric = {.max_rate_hz = 0.0},
          .proprioception = {.max_rate_hz = 0.0},
          .sensor = {.max_rate_hz = 250.0},
          .health = {.max_rate_hz = 10.0}};
}

std::optional<hardware::MicrophoneTelemetry> AlsaMic::ReadMicrophoneTelemetry() const {
  if (impl_->Halted()) return std::nullopt;
  std::vector<std::int16_t> frames;
  std::optional<std::int64_t> source_timestamp_ns;
  {
    std::lock_guard<std::mutex> lock(impl_->window_mutex);
    frames = std::move(impl_->window);
    impl_->window.clear();
    source_timestamp_ns = impl_->source_timestamp_ns;
    impl_->source_timestamp_ns.reset();
  }
  if (frames.empty()) return std::nullopt;
  hardware::MicrophoneTelemetry telemetry;
  telemetry.sample_rate_hz = impl_->config.sample_rate_hz;
  telemetry.channels = impl_->config.channels;
  telemetry.frame_id = impl_->config.frame_id;
  telemetry.source_timestamp_ns = source_timestamp_ns;
  telemetry.source_clock = hardware::SourceClockKind::kSteadyBoot;
  telemetry.source_clock_id = "alsa-capture-thread";
  telemetry.pcm = std::move(frames);
  return telemetry;
}

hardware::BackendHealth AlsaMic::ReadHealth() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return {.alive = !impl_->state->stopping && impl_->state->delivering, .admitted = IsAdmitted()};
}

void AlsaMic::Clamp() {
  // A microphone has no actuation path, and capture is continuous, so there is
  // nothing to clamp: nothing the device does can be brought to a safer state
  // by this call, and a dropped window is refilled immediately anyway. Halt()
  // is the state that stops the device.
}

void AlsaMic::Halt() {
  {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    if (impl_->state->stopping) return;
    impl_->state->stopping = true;
    impl_->state->delivering = false;
  }
  impl_->Clear();
  impl_->state->wakeup.notify_all();
  eos::lib::LogEvent(eos::lib::GetLogger("microphone"), spdlog::level::warn,
                     "microphone_backend_halted", {{"device", impl_->config.device_id}});
}

}  // namespace eos::backends::alsa

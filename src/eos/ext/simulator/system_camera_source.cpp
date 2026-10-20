#include "system_camera_source.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "src/eos/lib/log.h"

namespace eos::ext::simulator {
namespace {

constexpr std::size_t kMaximumFrameBytes = 8U * 1024U * 1024U;
constexpr auto kRetryInterval = std::chrono::seconds(1);

bool OpenSystemCamera(const SimulatorCameraConfig& config, CameraFrameStream* stream) {
  int output[2];
  if (::pipe(output) != 0) return false;
  int errors[2];
  if (::pipe(errors) != 0) {
    ::close(output[0]);
    ::close(output[1]);
    return false;
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(output[0]);
    ::close(output[1]);
    ::close(errors[0]);
    ::close(errors[1]);
    return false;
  }
  if (child == 0) {
    ::dup2(output[1], STDOUT_FILENO);
    ::dup2(errors[1], STDERR_FILENO);
    ::close(output[0]);
    ::close(output[1]);
    ::close(errors[0]);
    ::close(errors[1]);
    const std::string caps = "video/x-raw,width=" + std::to_string(config.width_px) +
                             ",height=" + std::to_string(config.height_px) +
                             ",framerate=" + std::to_string(config.fps) + "/1";
    ::execl(EOS_GSTREAMER_LAUNCH, EOS_GSTREAMER_LAUNCH, "-q", "autovideosrc", "!", "videoconvert",
            "!", caps.c_str(), "!", "jpegenc", "quality=90", "!", "fdsink", "fd=1",
            static_cast<char*>(nullptr));
    _exit(127);
  }

  ::close(output[1]);
  ::close(errors[1]);
  const int error_flags = ::fcntl(errors[0], F_GETFL, 0);
  if (error_flags >= 0) (void)::fcntl(errors[0], F_SETFL, error_flags | O_NONBLOCK);
  struct Process {
    pid_t child{-1};
    int output{-1};
    int errors{-1};
    std::vector<std::uint8_t> pending;
    std::string diagnostic;

    void DrainDiagnostics() {
      std::array<char, 1024> chunk{};
      while (errors >= 0) {
        const ssize_t count = ::read(errors, chunk.data(), chunk.size());
        if (count <= 0) return;
        constexpr std::size_t kMaximumDiagnosticBytes = 4096;
        const std::size_t available = kMaximumDiagnosticBytes - diagnostic.size();
        diagnostic.append(chunk.data(), std::min(available, static_cast<std::size_t>(count)));
      }
    }
  };
  auto process = std::make_shared<Process>();
  process->child = child;
  process->output = output[0];
  process->errors = errors[0];

  stream->read = [process](std::vector<std::uint8_t>* jpeg) {
    static constexpr std::array<std::uint8_t, 2> kStart{0xff, 0xd8};
    static constexpr std::array<std::uint8_t, 2> kEnd{0xff, 0xd9};
    auto extract = [&]() {
      auto start = std::search(process->pending.begin(), process->pending.end(), kStart.begin(),
                               kStart.end());
      if (start == process->pending.end()) {
        if (process->pending.size() > 1)
          process->pending.erase(process->pending.begin(), process->pending.end() - 1);
        return false;
      }
      if (start != process->pending.begin())
        process->pending.erase(process->pending.begin(), start);
      auto end = std::search(process->pending.begin() + 2, process->pending.end(), kEnd.begin(),
                             kEnd.end());
      if (end == process->pending.end()) return false;
      jpeg->assign(process->pending.begin(), end + 2);
      process->pending.erase(process->pending.begin(), end + 2);
      return true;
    };
    if (extract()) return CameraReadResult::kFrame;
    if (process->pending.size() > kMaximumFrameBytes) return CameraReadResult::kEnd;

    pollfd entries[2]{{.fd = process->output, .events = POLLIN, .revents = 0},
                      {.fd = process->errors, .events = POLLIN, .revents = 0}};
    const int ready = ::poll(entries, 2, 200);
    if (ready == 0) return CameraReadResult::kTimeout;
    if (ready < 0) return errno == EINTR ? CameraReadResult::kTimeout : CameraReadResult::kEnd;
    if (entries[1].revents != 0) process->DrainDiagnostics();
    if ((entries[0].revents & (POLLERR | POLLNVAL)) != 0) return CameraReadResult::kEnd;
    if ((entries[0].revents & (POLLIN | POLLHUP)) == 0) return CameraReadResult::kTimeout;

    std::array<std::uint8_t, 64U * 1024U> chunk{};
    const ssize_t count = ::read(process->output, chunk.data(), chunk.size());
    if (count < 0) return errno == EINTR ? CameraReadResult::kTimeout : CameraReadResult::kEnd;
    if (count == 0) return CameraReadResult::kEnd;
    process->pending.insert(process->pending.end(), chunk.begin(), chunk.begin() + count);
    if (process->pending.size() > kMaximumFrameBytes) return CameraReadResult::kEnd;
    return extract() ? CameraReadResult::kFrame : CameraReadResult::kTimeout;
  };
  stream->stop = [process] {
    if (process->output >= 0) {
      ::close(process->output);
      process->output = -1;
    }
    if (process->child > 0) {
      (void)::kill(process->child, SIGTERM);
      int status = 0;
      while (::waitpid(process->child, &status, 0) < 0 && errno == EINTR) {
      }
      process->child = -1;
    }
    process->DrainDiagnostics();
    if (process->errors >= 0) {
      ::close(process->errors);
      process->errors = -1;
    }
  };
  stream->diagnostics = [process] { return process->diagnostic; };
  return true;
}

}  // namespace

struct SystemCameraSource::Impl {
  explicit Impl(SimulatorCameraConfig supplied_config, CameraStreamOpen supplied_open)
      : config(supplied_config),
        open(supplied_open ? std::move(supplied_open) : CameraStreamOpen(OpenSystemCamera)),
        worker([this] { Run(); }) {}

  ~Impl() {
    {
      std::lock_guard lock(state_mutex);
      stopping = true;
    }
    wakeup.notify_all();
    if (worker.joinable()) worker.join();
  }

  void Run() {
    bool failure_logged = false;
    while (true) {
      {
        std::lock_guard lock(state_mutex);
        if (stopping) return;
      }
      CameraFrameStream stream;
      if (open(config, &stream) && stream.read && stream.stop) {
        bool delivered = false;
        while (true) {
          {
            std::lock_guard lock(state_mutex);
            if (stopping) break;
          }
          std::vector<std::uint8_t> jpeg;
          const CameraReadResult result = stream.read(&jpeg);
          if (result == CameraReadResult::kTimeout) continue;
          if (result == CameraReadResult::kEnd) break;
          const auto dimensions = JpegDimensions(jpeg);
          if (!dimensions || jpeg.size() > kMaximumFrameBytes) continue;
          if (dimensions->first != config.width_px || dimensions->second != config.height_px) {
            lib::LogEvent(lib::GetLogger("simulator.camera"), spdlog::level::warn,
                          "simulator_system_camera_mode_mismatch",
                          {{"requested_width_px", config.width_px},
                           {"requested_height_px", config.height_px},
                           {"actual_width_px", dimensions->first},
                           {"actual_height_px", dimensions->second}});
            break;
          }
          CapturedCameraFrame captured{
              .frame = {.width_px = dimensions->first,
                        .height_px = dimensions->second,
                        .jpeg = std::move(jpeg)},
              .source_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count()};
          {
            std::lock_guard lock(frame_mutex);
            frame = std::move(captured);
          }
          if (!delivered) {
            delivered = true;
            failure_logged = false;
            lib::LogEvent(lib::GetLogger("simulator.camera"), spdlog::level::info,
                          "simulator_system_camera_started");
          }
        }
        stream.stop();
        {
          std::lock_guard lock(frame_mutex);
          frame.reset();
        }
        {
          std::lock_guard lock(state_mutex);
          if (stopping) return;
        }
        if (!failure_logged) {
          const std::string reason = stream.diagnostics ? stream.diagnostics() : std::string{};
          lib::LogEvent(lib::GetLogger("simulator.camera"), spdlog::level::warn,
                        "simulator_system_camera_unavailable",
                        {{"reason", reason.empty() ? "capture process ended" : reason},
                         {"retry_ms", kRetryInterval.count() * 1000}});
          failure_logged = true;
        }
      } else if (!failure_logged) {
        lib::LogEvent(lib::GetLogger("simulator.camera"), spdlog::level::warn,
                      "simulator_system_camera_unavailable",
                      {{"reason", "capture process could not start"},
                       {"retry_ms", kRetryInterval.count() * 1000}});
        failure_logged = true;
      }

      std::unique_lock lock(state_mutex);
      if (stopping) return;
      wakeup.wait_for(lock, kRetryInterval, [&] { return stopping; });
    }
  }

  SimulatorCameraConfig config;
  CameraStreamOpen open;
  mutable std::mutex frame_mutex;
  std::optional<CapturedCameraFrame> frame;
  std::mutex state_mutex;
  std::condition_variable wakeup;
  bool stopping{false};
  std::thread worker;
};

SystemCameraSource::SystemCameraSource(SimulatorCameraConfig config, CameraStreamOpen open)
    : impl_(std::make_unique<Impl>(config, std::move(open))) {}

SystemCameraSource::~SystemCameraSource() = default;

std::optional<CapturedCameraFrame> SystemCameraSource::ReadFrame() const {
  std::lock_guard lock(impl_->frame_mutex);
  return impl_->frame;
}

}  // namespace eos::ext::simulator

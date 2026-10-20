#include "v4l2_cam.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "src/eos/lib/log.h"

namespace eos::backends::v4l2 {
namespace {

constexpr auto kRetryInterval = std::chrono::seconds(1);
constexpr std::uint32_t kBufferCount = 4;

/// One mmap'd kernel buffer.
struct Mapping {
  void* start{nullptr};
  std::size_t length{0};
};

void ReleaseBuffers(std::vector<Mapping>& mappings) {
  for (Mapping& mapping : mappings) {
    if (mapping.start != nullptr && mapping.start != MAP_FAILED) {
      ::munmap(mapping.start, mapping.length);
    }
    mapping.start = nullptr;
  }
  mappings.clear();
}

void CloseDevice(int fd, std::vector<Mapping>& mappings) {
  if (fd < 0) return;
  const v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  (void)::ioctl(fd, VIDIOC_STREAMOFF, &type);
  ReleaseBuffers(mappings);
  ::close(fd);
}

/// Whether the device can supply JPEG at all. An encoder does not exist in this
/// backend, so a camera limited to raw formats has nothing to contribute.
bool SupportsMjpeg(int fd) {
  v4l2_fmtdesc format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  for (format.index = 0; ::ioctl(fd, VIDIOC_ENUM_FMT, &format) == 0; ++format.index) {
    if (format.pixelformat == V4L2_PIX_FMT_MJPEG) return true;
  }
  return false;
}

/// Opens `config.video_device` and streams MJPEG frames through `stream`.
///
/// Every failure path returns false rather than throwing: a camera that cannot
/// be opened is an absent device, which the caller reports as such.
bool OpenV4l2(const V4l2CamConfig& config, FrameStream* stream) {
  const int fd = ::open(config.video_device.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) return false;

  v4l2_capability capability{};
  if (::ioctl(fd, VIDIOC_QUERYCAP, &capability) != 0 ||
      (capability.device_caps & V4L2_CAP_VIDEO_CAPTURE) == 0 ||
      (capability.device_caps & V4L2_CAP_STREAMING) == 0) {
    ::close(fd);
    return false;
  }
  if (!SupportsMjpeg(fd)) {
    eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn, "camera_no_mjpeg",
                       {{"device", config.device_id}, {"video_device", config.video_device}});
    ::close(fd);
    return false;
  }

  v4l2_format format{};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  format.fmt.pix.width = config.width_px;
  format.fmt.pix.height = config.height_px;
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
  format.fmt.pix.field = V4L2_FIELD_ANY;
  if (::ioctl(fd, VIDIOC_S_FMT, &format) != 0 || format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
      format.fmt.pix.width != config.width_px || format.fmt.pix.height != config.height_px) {
    eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn, "camera_mode_mismatch",
                       {{"device", config.device_id},
                        {"requested_width_px", config.width_px},
                        {"requested_height_px", config.height_px},
                        {"actual_width_px", format.fmt.pix.width},
                        {"actual_height_px", format.fmt.pix.height}});
    ::close(fd);
    return false;
  }

  v4l2_streamparm parameters{};
  parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (::ioctl(fd, VIDIOC_G_PARM, &parameters) == 0 &&
      (parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) != 0) {
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = config.fps == 0 ? 1 : config.fps;
    if (::ioctl(fd, VIDIOC_S_PARM, &parameters) != 0) {
      ::close(fd);
      return false;
    }
  }
  if (::ioctl(fd, VIDIOC_G_PARM, &parameters) != 0 ||
      parameters.parm.capture.timeperframe.numerator == 0 ||
      parameters.parm.capture.timeperframe.denominator !=
          config.fps * parameters.parm.capture.timeperframe.numerator) {
    eos::lib::LogEvent(
        eos::lib::GetLogger("camera"), spdlog::level::warn, "camera_rate_mismatch",
        {{"device", config.device_id},
         {"requested_fps", config.fps},
         {"actual_rate_numerator", parameters.parm.capture.timeperframe.denominator},
         {"actual_rate_denominator", parameters.parm.capture.timeperframe.numerator}});
    ::close(fd);
    return false;
  }

  v4l2_requestbuffers request{};
  request.count = kBufferCount;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (::ioctl(fd, VIDIOC_REQBUFS, &request) != 0 || request.count == 0) {
    ::close(fd);
    return false;
  }

  auto mappings = std::make_shared<std::vector<Mapping>>();
  mappings->resize(request.count);
  for (std::uint32_t index = 0; index < request.count; ++index) {
    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (::ioctl(fd, VIDIOC_QUERYBUF, &buffer) != 0) {
      CloseDevice(fd, *mappings);
      return false;
    }
    void* start =
        ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
    if (start == MAP_FAILED) {
      CloseDevice(fd, *mappings);
      return false;
    }
    (*mappings)[index] = Mapping{start, buffer.length};
    if (::ioctl(fd, VIDIOC_QBUF, &buffer) != 0) {
      CloseDevice(fd, *mappings);
      return false;
    }
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (::ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
    CloseDevice(fd, *mappings);
    return false;
  }

  stream->width_px = format.fmt.pix.width;
  stream->height_px = format.fmt.pix.height;
  stream->fps = config.fps;

  auto handle = std::make_shared<int>(fd);
  const int timeout_ms = static_cast<int>(config.read_timeout_ms);

  stream->read = [handle, mappings, timeout_ms](std::vector<std::uint8_t>* jpeg) {
    for (;;) {
      pollfd entry{};
      entry.fd = *handle;
      entry.events = POLLIN;
      const int ready = ::poll(&entry, 1, timeout_ms);
      if (ready <= 0) return false;
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      if (::ioctl(*handle, VIDIOC_DQBUF, &buffer) != 0) {
        // EAGAIN is the driver asking to be polled again, not a failure.
        if (errno == EAGAIN) continue;
        return false;
      }
      const Mapping& mapping = (*mappings)[buffer.index];
      const std::uint8_t* start = static_cast<const std::uint8_t*>(mapping.start);
      jpeg->assign(start, start + buffer.bytesused);
      (void)::ioctl(*handle, VIDIOC_QBUF, &buffer);
      return !jpeg->empty();
    }
  };
  stream->stop = [handle, mappings] {
    CloseDevice(*handle, *mappings);
    *handle = -1;
  };
  return true;
}

}  // namespace

struct V4l2Cam::Impl {
  struct State {
    std::mutex mutex;
    std::condition_variable wakeup;
    bool stopping{false};
    /// True once a frame has actually been delivered, so an opened-but-dead
    /// device never reads as alive.
    bool delivering{false};
  };

  V4l2CamConfig config;
  FrameOpen open;
  std::shared_ptr<State> state{std::make_shared<State>()};
  mutable std::mutex frame_mutex;
  std::vector<std::uint8_t> frame;
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::optional<std::int64_t> source_timestamp_ns;
  std::thread worker;

  Impl(V4l2CamConfig supplied, FrameOpen supplied_open)
      : config(std::move(supplied)),
        open(supplied_open ? std::move(supplied_open) : FrameOpen(OpenV4l2)) {
    if (config.device_id.empty() || config.video_device.empty() || config.frame_id.empty() ||
        config.width_px == 0 || config.height_px == 0 || config.read_timeout_ms == 0 ||
        config.fps == 0 || config.max_frame_bytes == 0 || !open) {
      throw std::invalid_argument("invalid v4l2 camera configuration");
    }
    width_px = config.width_px;
    height_px = config.height_px;
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

  void Publish(std::vector<std::uint8_t> jpeg, std::uint32_t width, std::uint32_t height) {
    std::lock_guard<std::mutex> lock(frame_mutex);
    if (jpeg.size() > config.max_frame_bytes) {
      frame.clear();
      source_timestamp_ns.reset();
      return;
    }
    frame = std::move(jpeg);
    width_px = width;
    height_px = height;
    source_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(frame_mutex);
    frame.clear();
    source_timestamp_ns.reset();
  }

  void Run(std::shared_ptr<State> shared) {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(shared->mutex);
        if (shared->stopping) return;
      }

      FrameStream stream;
      if (open(config, &stream) && stream.read && stream.stop) {
        const auto cancelled = [shared] {
          std::lock_guard<std::mutex> lock(shared->mutex);
          return shared->stopping;
        };
        if (stream.width_px != config.width_px || stream.height_px != config.height_px ||
            stream.fps != config.fps) {
          eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn,
                             "camera_mode_mismatch",
                             {{"device", config.device_id},
                              {"requested_width_px", config.width_px},
                              {"requested_height_px", config.height_px},
                              {"requested_fps", config.fps},
                              {"actual_width_px", stream.width_px},
                              {"actual_height_px", stream.height_px},
                              {"actual_fps", stream.fps}});
          stream.stop();
          Clear();
          std::lock_guard<std::mutex> lock(shared->mutex);
          shared->delivering = false;
          goto retry;
        }
        const std::uint32_t frame_width = stream.width_px;
        const std::uint32_t frame_height = stream.height_px;
        bool delivered = false;
        for (;;) {
          if (cancelled()) break;
          std::vector<std::uint8_t> jpeg;
          if (!stream.read(&jpeg)) break;
          Publish(std::move(jpeg), frame_width, frame_height);
          {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->delivering = true;
          }
          if (!delivered) {
            delivered = true;
            eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::info,
                               "camera_capture_started",
                               {{"device", config.device_id},
                                {"width_px", frame_width},
                                {"height_px", frame_height}});
          }
        }
        stream.stop();
        Clear();
        {
          std::lock_guard<std::mutex> lock(shared->mutex);
          shared->delivering = false;
        }
        if (!cancelled()) {
          eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn,
                             "camera_capture_interrupted",
                             {{"device", config.device_id},
                              {"delivered", delivered},
                              {"retry_ms", kRetryInterval.count() * 1000}});
        }
      } else {
        eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn, "camera_open_failed",
                           {{"device", config.device_id},
                            {"video_device", config.video_device},
                            {"retry_ms", kRetryInterval.count() * 1000}});
      }

    retry:
      std::unique_lock<std::mutex> lock(shared->mutex);
      if (shared->stopping) return;
      // Wait before retrying so a camera attached later is picked up without
      // spinning, and so shutdown stays prompt.
      shared->wakeup.wait_for(lock, kRetryInterval, [&] { return shared->stopping; });
    }
  }
};

V4l2Cam::V4l2Cam(V4l2CamConfig config, FrameOpen open)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(open))) {
  eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::info, "camera_backend_started",
                     {{"device", impl_->config.device_id},
                      {"video_device", impl_->config.video_device},
                      {"width_px", impl_->config.width_px},
                      {"height_px", impl_->config.height_px},
                      {"fps", impl_->config.fps}});
}

V4l2Cam::~V4l2Cam() {
  const std::string device = impl_->config.device_id;
  impl_.reset();
  eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::info, "camera_backend_stopped",
                     {{"device", device}});
}

hardware::BackendIdentity V4l2Cam::Identify() const {
  return {.name = "v4l2-cam",
          .version = "1.0.0",
          .device_id = impl_->config.device_id,
          .kind = hardware::DeviceKind::kCamera};
}

hardware::FeedDeclarations V4l2Cam::DeclareFeeds() const {
  return {.geometric = {.max_rate_hz = 0.0},
          .proprioception = {.max_rate_hz = 0.0},
          .sensor = {.max_rate_hz = static_cast<double>(impl_->config.fps)},
          .health = {.max_rate_hz = 10.0}};
}

std::optional<hardware::CameraTelemetry> V4l2Cam::ReadCameraTelemetry() const {
  if (impl_->Halted()) return std::nullopt;
  std::vector<std::uint8_t> jpeg;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::optional<std::int64_t> source_timestamp_ns;
  {
    std::lock_guard<std::mutex> lock(impl_->frame_mutex);
    jpeg = impl_->frame;
    width = impl_->width_px;
    height = impl_->height_px;
    source_timestamp_ns = impl_->source_timestamp_ns;
  }
  if (jpeg.empty()) return std::nullopt;
  hardware::CameraTelemetry telemetry;
  telemetry.width_px = width;
  telemetry.height_px = height;
  telemetry.frame_id = impl_->config.frame_id;
  telemetry.source_timestamp_ns = source_timestamp_ns;
  telemetry.source_clock = hardware::SourceClockKind::kSteadyBoot;
  telemetry.source_clock_id = "v4l2-capture-thread";
  telemetry.jpeg = std::move(jpeg);
  return telemetry;
}

hardware::BackendHealth V4l2Cam::ReadHealth() const {
  std::lock_guard<std::mutex> lock(impl_->state->mutex);
  return {.alive = !impl_->state->stopping && impl_->state->delivering, .admitted = IsAdmitted()};
}

void V4l2Cam::Clamp() {
  // A camera has no actuation path, and capture is continuous, so there is
  // nothing to clamp: nothing the device does can be brought to a safer state
  // by this call, and a dropped frame is replaced immediately anyway. Halt() is
  // the state that stops the device.
}

void V4l2Cam::Halt() {
  {
    std::lock_guard<std::mutex> lock(impl_->state->mutex);
    if (impl_->state->stopping) return;
    impl_->state->stopping = true;
    impl_->state->delivering = false;
  }
  impl_->Clear();
  impl_->state->wakeup.notify_all();
  eos::lib::LogEvent(eos::lib::GetLogger("camera"), spdlog::level::warn, "camera_backend_halted",
                     {{"device", impl_->config.device_id}});
}

}  // namespace eos::backends::v4l2

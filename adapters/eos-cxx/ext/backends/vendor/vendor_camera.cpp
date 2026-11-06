#include "vendor_camera.h"

#include <turbojpeg.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <librealsense2/rs.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/eos-cxx/lib/log.h"
#include "wrist_grasp_stream.h"


namespace eos::backends::vendor {
namespace {

constexpr std::size_t kMaximumFrameBytes = 4 * 1024 * 1024;
constexpr int kCaptureFramesPerSecond = 30;

auto VendorLogger() { return eos::lib::GetLogger("vendor"); }


std::vector<std::uint16_t> CopyAlignedDepth(const rs2::depth_frame& depth) {
  const int width = depth.get_width();
  const int height = depth.get_height();
  if (width <= 0 || height <= 0 || depth.get_bytes_per_pixel() != 2 ||
      depth.get_data() == nullptr) {
    return {};
  }

  const std::size_t row_bytes = static_cast<std::size_t>(width) * sizeof(std::uint16_t);
  const std::size_t stride = static_cast<std::size_t>(depth.get_stride_in_bytes());
  if (stride < row_bytes) return {};
  std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    std::memcpy(
        pixels.data() + static_cast<std::size_t>(y) * width,
        static_cast<const std::uint8_t*>(depth.get_data()) + static_cast<std::size_t>(y) * stride,
        row_bytes);
  }

  return pixels;
}

std::string DistortionModelName(rs2_distortion model) {
  switch (model) {
    case RS2_DISTORTION_NONE:
      return "none";
    case RS2_DISTORTION_MODIFIED_BROWN_CONRADY:
      return "modified_brown_conrady";
    case RS2_DISTORTION_INVERSE_BROWN_CONRADY:
      return "inverse_brown_conrady";
    case RS2_DISTORTION_FTHETA:
      return "ftheta";
    case RS2_DISTORTION_BROWN_CONRADY:
      return "brown_conrady";
    case RS2_DISTORTION_KANNALA_BRANDT4:
      return "kannala_brandt4";
    case RS2_DISTORTION_COUNT:
      break;
  }
  return "unknown";
}

hardware::CameraIntrinsics CameraIntrinsicsFrom(const rs2_intrinsics& value) {
  hardware::CameraIntrinsics output{
      .width_px = static_cast<std::uint32_t>(value.width),
      .height_px = static_cast<std::uint32_t>(value.height),
      .fx = value.fx,
      .fy = value.fy,
      .ppx = value.ppx,
      .ppy = value.ppy,
      .distortion_model = DistortionModelName(value.model)};
  for (std::size_t i = 0; i < output.distortion_coefficients.size(); ++i) {
    output.distortion_coefficients[i] = value.coeffs[i];
  }
  return output;
}

class RealSenseCameraDriver final : public VendorCameraDriver {
 public:
  RealSenseCameraDriver() {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    const std::string base = runtime_dir && *runtime_dir ? runtime_dir : "/tmp";
    const std::string lock_dir = base + "/island-arm-hardware-" + std::to_string(getuid());
    std::filesystem::create_directories(lock_dir);
    const std::string lock_path = lock_dir + "/d435.lock";
    camera_lock_.value = open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (camera_lock_.value < 0 || flock(camera_lock_.value, LOCK_EX | LOCK_NB) != 0) {
      throw std::runtime_error("D435 is already owned by another launcher: " + lock_path);
    }
    config_.enable_stream(RS2_STREAM_COLOR, 1920, 1080, RS2_FORMAT_BGR8, kCaptureFramesPerSecond);
    config_.enable_stream(RS2_STREAM_DEPTH, 1280, 720, RS2_FORMAT_Z16, kCaptureFramesPerSecond);
    rs2::pipeline_profile profile;
    try {
      profile = pipeline_.start(config_);
    } catch (const rs2::error& error) {
      throw std::runtime_error(
          "RealSense could not start color=1920x1080:BGR8 and depth=1280x720:Z16 at " +
          std::to_string(kCaptureFramesPerSecond) + " Hz: " + error.what() +
          " (function=" + error.get_failed_function() + ", args=" + error.get_failed_args() + ")");
    }
    depth_scale_ = profile.get_device().first<rs2::depth_sensor>().get_depth_scale();
    depth_intrinsics_ = CameraIntrinsicsFrom(
        profile.get_stream(RS2_STREAM_COLOR).as<rs2::video_stream_profile>().get_intrinsics());
    try {
      grasp_stream_ = std::make_unique<WristGraspStream>();
    } catch (const std::exception& error) {
      eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "wrist_grasp_stream_failed",
                         {{"reason", error.what()}});
    }
    compressor_ = tjInitCompress();
    if (compressor_ == nullptr) {
      pipeline_.stop();
      throw std::runtime_error("TurboJPEG initialization failed");
    }
  }

  ~RealSenseCameraDriver() override {
    Stop();
    if (compressor_ != nullptr) tjDestroy(compressor_);
  }

  std::optional<hardware::CameraTelemetry> Capture() override {
    const rs2::frameset frames = align_to_color_.process(pipeline_.wait_for_frames());
    const rs2::video_frame color = frames.get_color_frame();
    const rs2::depth_frame depth = frames.get_depth_frame();
    if (!color || !depth || color.get_width() != depth.get_width() ||
        color.get_height() != depth.get_height()) {
      return std::nullopt;
    }
    unsigned char* jpeg = nullptr;
    unsigned long jpeg_size = 0;
    const int width = color.get_width();
    const int height = color.get_height();
    if (tjCompress2(compressor_, static_cast<const unsigned char*>(color.get_data()), width,
                    width * 3, height, TJPF_BGR, &jpeg, &jpeg_size, TJSAMP_420, 85,
                    TJFLAG_FASTDCT) != 0 ||
        jpeg == nullptr || jpeg_size == 0) {
      if (jpeg != nullptr) tjFree(jpeg);
      throw std::runtime_error(std::string("TurboJPEG compression failed: ") + tjGetErrorStr());
    }
    hardware::CameraTelemetry frame{
        .width_px = static_cast<std::uint32_t>(width),
        .height_px = static_cast<std::uint32_t>(height),
        .frame_id = "camera_link",
        .source_timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count(),
        .source_clock = hardware::SourceClockKind::kUnix,
        .source_clock_id = "system",
        .jpeg = std::vector<std::uint8_t>(jpeg, jpeg + jpeg_size),
        .depth_u16 = CopyAlignedDepth(depth),
        .depth_scale = depth_scale_,
        .depth_intrinsics = depth_intrinsics_};
    tjFree(jpeg);
    if (frame.depth_u16.empty()) {
      throw std::runtime_error("RealSense aligned depth copy failed");
    }
    if (grasp_stream_) {
      try {
        grasp_stream_->Queue(color, depth, *frame.source_timestamp_ns, frame.jpeg);
      } catch (const std::exception& error) {
        eos::lib::LogEvent(VendorLogger(), spdlog::level::warn, "wrist_grasp_stream_failed",
                           {{"reason", error.what()}});
      }
    }
    return frame;
  }

  void Stop() override {
    if (!started_) return;
    pipeline_.stop();
    if (grasp_stream_) grasp_stream_->Stop();
    started_ = false;
  }

 private:
  struct ScopedFd {
    int value{-1};
    ~ScopedFd() { if (value >= 0) close(value); }
  };

  rs2::pipeline pipeline_;
  rs2::config config_;
  rs2::align align_to_color_{RS2_STREAM_COLOR};
  tjhandle compressor_{nullptr};
  double depth_scale_{0.001};
  hardware::CameraIntrinsics depth_intrinsics_;
  bool started_{true};
  ScopedFd camera_lock_;
  std::unique_ptr<WristGraspStream> grasp_stream_;
};

}  // namespace

struct VendorCamera::Impl {
  Impl(std::string id, std::unique_ptr<VendorCameraDriver> value)
      : identity{.name = "vendor-camera",
                 .version = "1.0.0",
                 .device_id = std::move(id),
                 .kind = hardware::DeviceKind::kCamera},
        driver(std::move(value)) {}

  hardware::BackendIdentity identity;
  std::unique_ptr<VendorCameraDriver> driver;
  mutable std::mutex mutex;
  bool alive{true};
  bool halted{false};
};

VendorCamera::VendorCamera(std::string device_id)
    : VendorCamera(std::move(device_id), std::make_unique<RealSenseCameraDriver>()) {}

VendorCamera::VendorCamera(std::string device_id, std::unique_ptr<VendorCameraDriver> driver)
    : impl_(std::make_unique<Impl>(std::move(device_id), std::move(driver))) {
  if (impl_->driver == nullptr) throw std::invalid_argument("vendor camera driver is required");
  eos::lib::LogEvent(VendorLogger(), spdlog::level::info, "vendor_camera_started",
                     {{"device", impl_->identity.device_id}});
}

VendorCamera::~VendorCamera() { Halt(); }

hardware::BackendIdentity VendorCamera::Identify() const { return impl_->identity; }

hardware::FeedDeclarations VendorCamera::DeclareFeeds() const {
  return {.geometric = {.max_rate_hz = 0.0},
          .proprioception = {.max_rate_hz = 0.0},
          .sensor = {.max_rate_hz = static_cast<double>(kCaptureFramesPerSecond)},
          .health = {.max_rate_hz = 10.0}};
}

hardware::BackendHealth VendorCamera::ReadHealth() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return {.alive = impl_->alive, .admitted = IsAdmitted()};
}

std::optional<hardware::CameraTelemetry> VendorCamera::ReadCameraTelemetry() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->halted || !impl_->alive) return std::nullopt;
  try {
    std::optional<hardware::CameraTelemetry> frame = impl_->driver->Capture();
    if (!frame) return std::nullopt;
    if (frame->width_px == 0 || frame->height_px == 0 || frame->frame_id.empty() ||
        (frame->source_timestamp_ns && *frame->source_timestamp_ns < 0) || frame->jpeg.empty() ||
        frame->jpeg.size() > kMaximumFrameBytes ||
        (!frame->depth_u16.empty() &&
         frame->depth_u16.size() != static_cast<std::size_t>(frame->width_px) * frame->height_px)) {
      eos::lib::LogOnce(VendorLogger(), spdlog::level::warn,
                        impl_->identity.device_id + ":invalid_camera_telemetry",
                        "vendor_camera_frame_rejected",
                        {{"device", impl_->identity.device_id},
                         {"width_px", frame->width_px},
                         {"height_px", frame->height_px},
                         {"bytes", frame->jpeg.size()},
                         {"frame_id_present", !frame->frame_id.empty()}});
      return std::nullopt;
    }
    return frame;
  } catch (const std::exception& error) {
    impl_->alive = false;
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_camera_capture_failed",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
  } catch (...) {
    impl_->alive = false;
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_camera_capture_failed",
                       {{"device", impl_->identity.device_id}, {"reason", "unknown_exception"}});
  }
  return std::nullopt;
}

void VendorCamera::Clamp() {}

void VendorCamera::Halt() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->halted) return;
  impl_->halted = true;
  try {
    impl_->driver->Stop();
  } catch (const std::exception& error) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_camera_stop_failed",
                       {{"device", impl_->identity.device_id}, {"reason", error.what()}});
  } catch (...) {
    eos::lib::LogEvent(VendorLogger(), spdlog::level::err, "vendor_camera_stop_failed",
                       {{"device", impl_->identity.device_id}, {"reason", "unknown_exception"}});
  }
  impl_->alive = false;
}

}  // namespace eos::backends::vendor

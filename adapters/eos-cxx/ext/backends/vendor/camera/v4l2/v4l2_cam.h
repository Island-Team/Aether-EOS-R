/// Native Linux camera backend: captures a host camera into the bounded JPEG
/// frame the perception reader publishes.
///
/// This is the capture counterpart of the aplay speaker backend and the sibling
/// of the ALSA microphone: the device boundary is injected, so tests drive the
/// backend through a scripted stream and never need a camera.
///
/// Only cameras that can supply MJPEG are supported. A device that offers no
/// JPEG — or that is missing, busy, or otherwise unusable — is reported as
/// absence: `ReadCameraTelemetry()` returns nothing and health stays not alive,
/// so the northbound snapshot says the camera is not there instead of showing a
/// frame that was never captured. Synthesizing an image stays a launch/profile
/// decision rather than a hidden behavior of a device backend.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"

namespace eos::backends::v4l2 {

struct V4l2CamConfig {
  std::string device_id;
  std::string video_device{"/dev/video0"};
  std::string frame_id{"camera"};  ///< robot-description frame for captured pixels
  std::uint32_t width_px{640};     ///< requested capture width
  std::uint32_t height_px{480};    ///< requested capture height
  std::uint32_t fps{30};           ///< requested frame rate
  /// Bounds how long one frame may be waited for before the device is treated
  /// as stalled.
  std::uint32_t read_timeout_ms{2000};
  std::size_t max_frame_bytes{4 * 1024 * 1024};  ///< maximum accepted encoded JPEG size
};

/// One open capture stream. `Read` fills `jpeg` with one complete JPEG frame
/// and returns false at end of stream or on device error; `Stop` releases the
/// device and is safe to call more than once.
///
/// The opener fills the negotiated mode. The backend accepts frames only when
/// all three values exactly match the requested mode.
struct FrameStream {
  std::function<bool(std::vector<std::uint8_t>* jpeg)> read;
  std::function<void()> stop;
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::uint32_t fps{0};
};

/// Opens a capture stream for `config`. Returns false when the device is
/// missing, busy, has no MJPEG mode, or is otherwise unusable — the caller then
/// reports absence.
using FrameOpen = std::function<bool(const V4l2CamConfig& config, FrameStream* stream)>;

class V4l2Cam final : public hardware::Backend {
 public:
  V4l2Cam(V4l2CamConfig config, FrameOpen open = {});
  ~V4l2Cam() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  void Clamp() override;
  void Halt() override;

  /// The newest captured frame, or nothing while no device is delivering one.
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override;
  hardware::BackendHealth ReadHealth() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::v4l2

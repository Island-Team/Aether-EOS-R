#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace eos::backends::config {
/// `[device.backend.v4l2]`: Linux Video4Linux2 capture settings. Every key is
/// optional; the defaults are a 640x480 MJPEG stream at 30 fps.
struct V4l2CamDeviceConfig {
  std::string video_device{"/dev/video0"};         ///< capture device node
  std::string frame_id{"camera"};                  ///< robot-description optical frame
  std::uint32_t width_px{640};                     ///< requested capture width
  std::uint32_t height_px{480};                    ///< requested capture height
  std::uint32_t fps{30};                           ///< requested frame rate
  std::uint32_t read_timeout_ms{2000};             ///< bound on one stalled frame read
  std::uint64_t max_frame_bytes{4 * 1024 * 1024};  ///< accepted encoded-frame bound
};


}  // namespace eos::backends::config

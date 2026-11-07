/// Bounded decoding of demo-world image and video source assets.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eos::ext::simulator {

struct EncodedFrame {
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::vector<std::uint8_t> jpeg;
};

struct DecodedMedia {
  std::string id;
  bool video{false};
  double frames_per_second{30.0};
  std::vector<EncodedFrame> frames;
};

/// Returns JPEG dimensions after bounded marker validation.
std::optional<std::pair<std::uint32_t, std::uint32_t>> JpegDimensions(
    const std::vector<std::uint8_t>& bytes);

/// Decodes one validated scene asset into bounded JPEG camera frames. JPEG
/// images are retained without a lossy round trip; other images and video are
/// decoded by the configured GStreamer launcher.
DecodedMedia DecodeMedia(const std::string& id, const std::filesystem::path& path,
                         const std::string& media_type);

}  // namespace eos::ext::simulator

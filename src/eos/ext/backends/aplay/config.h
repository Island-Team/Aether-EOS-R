#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace eos::backends::config {
/// `[device.backend.aplay]`: Linux ALSA playback settings.
struct AplayDeviceConfig {
  std::string alsa_device{"default"};                ///< ALSA PCM route passed to aplay -D
  std::uint64_t max_stream_bytes{16 * 1024 * 1024};  ///< atomic audio asset bound
};


}  // namespace eos::backends::config

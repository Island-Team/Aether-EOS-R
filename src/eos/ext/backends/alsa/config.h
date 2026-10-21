#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace eos::backends::config {
/// `[device.backend.alsa]`: Linux ALSA capture settings. Every key is optional
/// because the defaults are the audio contracts' own: 16 kHz mono, 20 ms.
struct AlsaMicDeviceConfig {
  std::string alsa_device{"default"};   ///< ALSA PCM route passed to arecord -D
  std::string frame_id{"mic"};          ///< robot-description mounting frame
  std::uint32_t sample_rate_hz{16000};  ///< capture rate
  std::uint16_t channels{1};            ///< interleaved channel count
  std::uint32_t window_frames{320};     ///< frames per published window
  std::uint32_t read_timeout_ms{2000};  ///< bound on one stalled window read
};


}  // namespace eos::backends::config

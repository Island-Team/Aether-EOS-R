/// Native Linux microphone backend: captures the host's audio input into the
/// bounded PCM window the perception reader publishes.
///
/// This is the capture counterpart of the aplay speaker backend and follows the
/// same shape: the process boundary is injected, so tests drive the backend
/// through a scripted stream and never need a sound card.
///
/// A missing or unopenable device is reported as absence, not as an empty
/// window: `ReadMicrophoneTelemetry()` returns nothing and health stays not
/// alive, so the northbound snapshot says the device is not there instead of
/// claiming a capture that is not happening. Falling back to synthetic audio
/// stays a launch/profile decision rather than a hidden behavior of a device
/// backend.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"

namespace eos::backends::alsa {

struct AlsaMicConfig {
  std::string device_id;
  std::string alsa_device{"default"};
  std::string frame_id{"mic"};  ///< robot-description frame for captured audio
  std::uint32_t sample_rate_hz{16000};
  std::uint16_t channels{1};
  /// Frames in each published window. 320 frames is 20 ms at 16 kHz.
  std::uint32_t window_frames{320};
  /// Bounds how long one window read may block before it is treated as a
  /// stalled device.
  std::uint32_t read_timeout_ms{2000};
};

/// Reports whether the owner still wants samples. The capture loop stops when
/// it turns true.
using CaptureProbe = std::function<bool()>;

/// One open capture stream. `Read` fills exactly `frames_wanted` frames and
/// returns false at end of stream or on device error; `Stop` releases the
/// device and is safe to call more than once.
struct CaptureStream {
  std::function<bool(std::vector<std::int16_t>* frames, std::size_t frames_wanted)> read;
  std::function<void()> stop;
};

/// Opens a capture stream for `config`. Returns false when the device is
/// missing, busy, or otherwise unusable — the caller then reports absence.
using CaptureOpen = std::function<bool(const AlsaMicConfig& config, CaptureStream* stream)>;

class AlsaMic final : public hardware::Backend {
 public:
  AlsaMic(AlsaMicConfig config, CaptureOpen open = {});
  ~AlsaMic() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  void Clamp() override;
  void Halt() override;

  /// The newest captured window, or nothing while no device is delivering one.
  std::optional<hardware::MicrophoneTelemetry> ReadMicrophoneTelemetry() const override;
  hardware::BackendHealth ReadHealth() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::alsa

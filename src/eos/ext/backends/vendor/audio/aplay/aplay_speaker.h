/// Native Linux speaker backend for atomic bounded WAV playback through ALSA.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "eos/hardware/backend.h"

namespace eos::backends::aplay {

struct AplaySpeakerConfig {
  std::string device_id;
  std::string alsa_device{"default"};
  std::size_t max_stream_bytes{16 * 1024 * 1024};
};

/// Outcome of one playback attempt. A failed attempt carries the cause so it
/// reaches operators instead of collapsing into an unattributed fault.
struct PlaybackOutcome {
  bool ok{false};
  std::string reason;
};

using CancellationProbe = std::function<bool()>;
using Playback = std::function<PlaybackOutcome(const std::vector<std::uint8_t>&, const std::string&,
                                               std::chrono::milliseconds,
                                               const CancellationProbe&)>;

class AplaySpeaker final : public hardware::Backend {
 public:
  explicit AplaySpeaker(AplaySpeakerConfig config, Playback playback = {});
  ~AplaySpeaker() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::ReleaseResult ExecuteSpeaker(const hardware::SpeakerCommand& command) override;
  std::optional<hardware::SpeakerTelemetry> ReadSpeakerTelemetry() const override;
  hardware::BackendHealth ReadHealth() const override;
  /// Cause of the most recent failed playback, or the backend default when the
  /// last playback succeeded.
  std::string ReleaseFailureReason() const override;
  void Clamp() override;
  void Halt() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::aplay

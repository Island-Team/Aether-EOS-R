#include "demo_speaker.h"

#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoSpeaker::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;
  std::optional<std::string> last_asset_id;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-speaker",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kSpeaker},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 10.0},
                                .sensor = {.max_rate_hz = 0.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver) {}
};

DemoSpeaker::DemoSpeaker(std::string device_id)
    : impl_(new Impl(std::move(device_id), std::make_shared<ext::simulator::ipc::SimulatorClient>(
                                               "var/run/simulator.sock"))) {}

DemoSpeaker::DemoSpeaker(std::string device_id,
                         std::shared_ptr<ext::simulator::ipc::SimulatorClient> world)
    : impl_(new Impl(std::move(device_id), std::move(world))) {}

DemoSpeaker::~DemoSpeaker() { delete impl_; }

hardware::BackendIdentity DemoSpeaker::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoSpeaker::DeclareFeeds() const { return impl_->core.feeds(); }

hardware::ReleaseResult DemoSpeaker::ExecuteSpeaker(const hardware::SpeakerCommand& cmd) {
  if (impl_->core.TryRelease() != hardware::ReleaseResult::kAccepted) {
    return hardware::ReleaseResult::kRejected;
  }
  constexpr std::size_t kMaximumStreamBytes = 16 * 1024 * 1024;
  if (cmd.asset_id.empty() || cmd.asset_id.size() > 2048 || cmd.media_type != "audio/wav" ||
      cmd.data.empty() || cmd.data.size() > kMaximumStreamBytes ||
      impl_->driver->ReadSpeaker(impl_->core.identity().device_id)
          .value_or(ext::simulator::ipc::SpeakerState{})
          .playing) {
    return hardware::ReleaseResult::kRejected;
  }
  impl_->last_asset_id = cmd.asset_id;
  return impl_->driver->PlayAudio(impl_->core.identity().device_id, cmd.data.size())
             ? hardware::ReleaseResult::kAccepted
             : hardware::ReleaseResult::kRejected;
}

void DemoSpeaker::Tick(double) {}

void DemoSpeaker::Clamp() { impl_->driver->StopSpeaker(impl_->core.identity().device_id); }

void DemoSpeaker::Halt() {
  Clamp();
  impl_->core.LatchHalt();
}

std::optional<hardware::SpeakerTelemetry> DemoSpeaker::ReadSpeakerTelemetry() const {
  const auto state = impl_->driver->ReadSpeaker(impl_->core.identity().device_id);
  if (!state) return std::nullopt;
  return hardware::SpeakerTelemetry{
      .state = state->playing ? hardware::SpeakerPlaybackState::kPlaying
                              : hardware::SpeakerPlaybackState::kIdle,
      .asset_id = impl_->last_asset_id,
      .queued_duration_ms = std::nullopt,
      .played_duration_ms = std::nullopt,
      .underruns = 0,
      .source_timestamp_ns = std::nullopt,
  };
}

DemoSpeakerHealth DemoSpeaker::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoSpeakerHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

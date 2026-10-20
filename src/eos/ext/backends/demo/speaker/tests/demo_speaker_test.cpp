/// Structure test: drives stub data through the demo speaker in the same
/// order the release chain does — admission, release, playback countdown,
/// fail-safe primitives — and reads the stub speaker feed back. No runtime
/// modules involved.
#include "demo_speaker.h"

#include <cassert>
#include <cstdio>
#include <string>

int main() {
  using namespace eos;
  backends::demo::DemoSpeaker speaker("demo-speaker-0");
  const hardware::BackendIdentity id = speaker.Identify();
  assert(id.name == "demo-speaker");
  assert(id.kind == hardware::DeviceKind::kSpeaker);
  assert(id.device_id == "demo-speaker-0");
  const hardware::FeedDeclarations feeds = speaker.DeclareFeeds();
  assert(feeds.proprioception.max_rate_hz == 10.0);
  assert(speaker.ReadHealth().alive && !speaker.ReadHealth().admitted);

  // Bare construction: idle, nothing played yet.
  std::optional<hardware::SpeakerTelemetry> s = speaker.ReadSpeakerTelemetry();
  assert(s.has_value());
  assert(s->state == hardware::SpeakerPlaybackState::kIdle);
  assert(!s->asset_id.has_value());

  // Wrong-kind releases are rejected.
  hardware::MoveLCommand cart{};
  assert(speaker.ExecuteMoveL(cart) == hardware::ReleaseResult::kRejected);
  hardware::BaseCommand base{};
  assert(speaker.ExecuteBase(base) == hardware::ReleaseResult::kRejected);

  // One release starts playback of one complete asset.
  hardware::SpeakerCommand cmd{
      .asset_id = "hello", .media_type = "audio/wav", .data = {1, 2, 3, 4, 5, 6}};
  assert(speaker.ExecuteSpeaker(cmd) == hardware::ReleaseResult::kAccepted);
  s = speaker.ReadSpeakerTelemetry();
  assert(s->state == hardware::SpeakerPlaybackState::kPlaying);
  assert(s->asset_id == "hello");
  hardware::SpeakerCommand overlapping{
      .asset_id = "overlap", .media_type = "audio/wav", .data = {7, 8}};
  assert(speaker.ExecuteSpeaker(overlapping) == hardware::ReleaseResult::kRejected);
  assert(speaker.ReadSpeakerTelemetry()->asset_id == "hello");

  // Tick partway: still playing, countdown advanced.
  speaker.Tick(0.1);
  s = speaker.ReadSpeakerTelemetry();
  assert(s->state == hardware::SpeakerPlaybackState::kPlaying);

  // Tick past the duration: playback clears, the stream record persists.
  speaker.Tick(0.5);
  s = speaker.ReadSpeakerTelemetry();
  assert(s->state == hardware::SpeakerPlaybackState::kIdle);
  assert(s->asset_id == "hello");

  // A new asset starts once playback is idle.
  cmd.asset_id = "stopping";
  cmd.data = {7, 8};
  assert(speaker.ExecuteSpeaker(cmd) == hardware::ReleaseResult::kAccepted);
  assert(speaker.ReadSpeakerTelemetry()->state == hardware::SpeakerPlaybackState::kPlaying);
  assert(speaker.ReadSpeakerTelemetry()->asset_id == "stopping");

  // Clamp: stop playing; the last-played record survives.
  speaker.Clamp();
  s = speaker.ReadSpeakerTelemetry();
  assert(s->state == hardware::SpeakerPlaybackState::kIdle);
  assert(s->asset_id == "stopping");

  // Halt: clamp plus the latch — every further release is rejected.
  cmd.asset_id = "after-halt";
  speaker.Halt();
  assert(speaker.ExecuteSpeaker(cmd) == hardware::ReleaseResult::kRejected);
  assert(!speaker.ReadHealth().alive);

  std::puts("demo speaker structure test: OK");
  return 0;
}

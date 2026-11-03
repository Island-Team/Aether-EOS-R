/// Structure test: drives the demo microphone through the south-bound
/// interface in bridge order — admission, attempted release (must reject:
/// feed-only device), fail-safe primitives — and reads the stub sensor feed
/// back.
#include "demo_mic.h"

#include <cassert>
#include <cstdio>

int main() {
  using namespace eos;

  backends::demo::DemoMic mic("demo-mic-0");
  const hardware::BackendIdentity id = mic.Identify();
  assert(id.name == "demo-mic");
  assert(id.kind == hardware::DeviceKind::kMicrophone);
  assert(id.device_id == "demo-mic-0");
  const hardware::FeedDeclarations feeds = mic.DeclareFeeds();
  assert(feeds.sensor.max_rate_hz == 250.0);

  hardware::MoveLCommand cmd{};
  assert(mic.ExecuteMoveL(cmd) == hardware::ReleaseResult::kRejected);
  hardware::BaseCommand base{};
  assert(mic.ExecuteBase(base) == hardware::ReleaseResult::kRejected);
  hardware::GimbalCommand gimbal{};
  assert(mic.ExecuteGimbal(gimbal) == hardware::ReleaseResult::kRejected);
  hardware::BaseNavigateRelativeCommand navigation{};
  assert(mic.ExecuteNavigateRelative(navigation) == hardware::ReleaseResult::kRejected);
  assert(mic.ExecutePosture({hardware::BasePosture::kStand, "posture"}) ==
         hardware::ReleaseResult::kRejected);
  assert(mic.StopMotion() == hardware::ReleaseResult::kRejected);

  const backends::demo::DemoMicBlock b = mic.ReadSensor();
  assert(b.sample_rate_hz == 16000);
  assert(b.channels == 1);
  assert(b.samples[1] == 512);

  // The backend-neutral seam the runtime reads: the same window, with no demo
  // world type crossing the boundary.
  const std::optional<hardware::MicrophoneTelemetry> t = mic.ReadMicrophoneTelemetry();
  assert(t.has_value());
  assert(t->sample_rate_hz == 16000);
  assert(t->channels == 1);
  assert(t->frame_id == "mic");
  assert(t->pcm.size() == 64u);
  assert(t->pcm[0] == 0);
  assert(t->pcm[1] == 512);
  assert(t->pcm[63] == 32256);

  mic.Clamp();
  assert(mic.ReadHealth().alive);
  mic.Halt();
  assert(!mic.ReadHealth().alive);
  assert(!mic.ReadMicrophoneTelemetry().has_value());

  std::puts("demo mic structure test: OK");
  return 0;
}

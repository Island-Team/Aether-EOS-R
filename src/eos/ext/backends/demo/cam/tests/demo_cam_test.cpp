/// Structure test: drives the demo camera through the south-bound interface
/// in bridge order — admission, attempted release (must reject: feed-only
/// device), fail-safe primitives — and reads the stub sensor feed back.
#include "demo_cam.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <utility>

int main() {
  using namespace eos;

  auto client = std::make_shared<ext::simulator::ipc::SimulatorClient>(
      std::filesystem::temp_directory_path() / "eos-missing-simulator.sock");
  backends::demo::DemoCam cam("demo-cam-0", client);
  const hardware::BackendIdentity id = cam.Identify();
  assert(id.name == "demo-cam");
  assert(id.kind == hardware::DeviceKind::kCamera);
  assert(id.device_id == "demo-cam-0");
  const hardware::FeedDeclarations feeds = cam.DeclareFeeds();
  assert(feeds.sensor.max_rate_hz == 30.0);
  assert(feeds.proprioception.max_rate_hz == 0.0);

  hardware::MoveLCommand cmd{};
  assert(cam.ExecuteMoveL(cmd) == hardware::ReleaseResult::kRejected);
  hardware::BaseCommand base{};
  assert(cam.ExecuteBase(base) == hardware::ReleaseResult::kRejected);
  hardware::GimbalCommand gimbal{};
  assert(cam.ExecuteGimbal(gimbal) == hardware::ReleaseResult::kRejected);
  hardware::BaseNavigateRelativeCommand navigation{};
  assert(cam.ExecuteNavigateRelative(navigation) == hardware::ReleaseResult::kRejected);
  assert(cam.ExecutePosture({hardware::BasePosture::kStand, "posture"}) ==
         hardware::ReleaseResult::kRejected);
  assert(cam.StopMotion() == hardware::ReleaseResult::kRejected);

  assert(!cam.ReadCameraTelemetry().has_value());

  cam.Clamp();
  assert(!cam.ReadHealth().alive);
  cam.Halt();
  assert(!cam.ReadHealth().alive);
  assert(!cam.ReadCameraTelemetry().has_value());
  assert(cam.ExecuteMoveL(cmd) == hardware::ReleaseResult::kRejected);

  std::puts("demo cam structure test: OK");
  return 0;
}

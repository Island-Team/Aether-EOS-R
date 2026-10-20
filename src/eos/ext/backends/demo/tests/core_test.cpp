/// Structure test: exercises the shared demo core once — admission data,
/// release latch, health semantics, halt latching — so the five device
/// tests only need to check their kind-specific wiring.
#include "core.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <utility>

int main() {
  using namespace eos;

  backends::demo::DemoSpec spec{};
  spec.identity = hardware::BackendIdentity{
      .name = "demo-arm",
      .version = "1.0.0",
      .device_id = "demo-arm-0",
      .kind = hardware::DeviceKind::kManipulator,
  };
  spec.feeds.health.max_rate_hz = 10.0;
  auto driver = std::make_shared<ext::simulator::ipc::SimulatorClient>(
      std::filesystem::temp_directory_path() / "eos-missing-simulator.sock");
  backends::demo::Core core(std::move(spec), driver);

  const hardware::BackendIdentity& id = core.identity();
  assert(id.name == "demo-arm");
  assert(id.device_id == "demo-arm-0");
  assert(core.feeds().health.max_rate_hz == 10.0);

  const backends::demo::DemoHealth h0 = core.ReadHealth();
  assert(!h0.alive && !h0.admitted);
  assert(core.TryRelease() == hardware::ReleaseResult::kRejected);

  core.LatchHalt();
  assert(core.halted());
  assert(core.TryRelease() == hardware::ReleaseResult::kRejected);
  const backends::demo::DemoHealth h1 = core.ReadHealth();
  assert(!h1.alive && !h1.admitted);

  std::puts("demo core structure test: OK");
  return 0;
}

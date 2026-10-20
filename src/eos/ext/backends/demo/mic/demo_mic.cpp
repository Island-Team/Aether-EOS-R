#include "demo_mic.h"

#include <iterator>
#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoMic::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-mic",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kMicrophone},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 0.0},
                                .sensor = {.max_rate_hz = 250.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver) {}
};

DemoMic::DemoMic(std::string device_id)
    : DemoMic(std::move(device_id),
              std::make_shared<ext::simulator::ipc::SimulatorClient>("var/run/simulator.sock")) {}

DemoMic::DemoMic(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world)
    : impl_(new Impl(std::move(device_id), std::move(world))) {}

DemoMic::~DemoMic() { delete impl_; }

hardware::BackendIdentity DemoMic::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoMic::DeclareFeeds() const { return impl_->core.feeds(); }

void DemoMic::Clamp() {}

void DemoMic::Halt() { impl_->core.LatchHalt(); }

DemoMicBlock DemoMic::ReadSensor() const {
  return impl_->driver->ReadMicrophone(impl_->core.identity().device_id).value_or(DemoMicBlock{});
}

std::optional<hardware::MicrophoneTelemetry> DemoMic::ReadMicrophoneTelemetry() const {
  if (impl_->core.halted()) return std::nullopt;
  const auto block = impl_->driver->ReadMicrophone(impl_->core.identity().device_id);
  if (!block) return std::nullopt;
  hardware::MicrophoneTelemetry telemetry;
  telemetry.sample_rate_hz = block->sample_rate_hz;
  telemetry.channels = block->channels;
  telemetry.frame_id = "mic";
  telemetry.pcm.assign(std::begin(block->samples), std::end(block->samples));
  return telemetry;
}

DemoMicHealth DemoMic::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoMicHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

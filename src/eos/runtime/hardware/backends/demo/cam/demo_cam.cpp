#include "demo_cam.h"

#include <utility>

#include "core.h"
#include "src/eos/ext/simulator/ipc/client.h"

namespace eos::backends::demo {

struct DemoCam::Impl {
  std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver;
  Core core;

  Impl(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> shared_world)
      : driver(std::move(shared_world)),
        core(DemoSpec{.identity = {.name = "demo-cam",
                                   .version = "1.0.0",
                                   .device_id = std::move(device_id),
                                   .kind = hardware::DeviceKind::kCamera},
                      .feeds = {.geometric = {.max_rate_hz = 0.0},
                                .proprioception = {.max_rate_hz = 0.0},
                                .sensor = {.max_rate_hz = 30.0},
                                .health = {.max_rate_hz = 10.0}}},
             driver) {}
};

DemoCam::DemoCam(std::string device_id)
    : DemoCam(std::move(device_id),
              std::make_shared<ext::simulator::ipc::SimulatorClient>("var/run/simulator.sock")) {}

DemoCam::DemoCam(std::string device_id, std::shared_ptr<ext::simulator::ipc::SimulatorClient> world)
    : impl_(new Impl(std::move(device_id), std::move(world))) {}

DemoCam::~DemoCam() { delete impl_; }

hardware::BackendIdentity DemoCam::Identify() const { return impl_->core.identity(); }

hardware::FeedDeclarations DemoCam::DeclareFeeds() const { return impl_->core.feeds(); }

std::optional<hardware::CameraTelemetry> DemoCam::ReadCameraTelemetry() const {
  if (impl_->core.halted()) return std::nullopt;
  const auto source = impl_->driver->ReadCameraSource(impl_->core.identity().device_id);
  if (!source) return std::nullopt;
  return hardware::CameraTelemetry{
      .width_px = source->width_px,
      .height_px = source->height_px,
      .frame_id = "demo-camera",
      .source_timestamp_ns = source->source_timestamp_ns,
      .jpeg = source->jpeg,
      .depth_png = {},
      .depth_scale = 0.001,
      .opaque_context = std::make_shared<const FrameContext>(FrameContext{.state = source->state})};
}

void DemoCam::Tick(double) {}

void DemoCam::Clamp() {}

void DemoCam::Halt() { impl_->core.LatchHalt(); }

DemoCamHealth DemoCam::ReadHealth() const {
  const DemoHealth h = impl_->core.ReadHealth();
  return DemoCamHealth{.alive = h.alive, .admitted = IsAdmitted()};
}

}  // namespace eos::backends::demo

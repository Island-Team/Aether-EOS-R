#include "core.h"

#include "src/eos/lib/log.h"

namespace eos::backends::demo {

Core::Core(DemoSpec spec, std::shared_ptr<ext::simulator::ipc::SimulatorClient> driver)
    : spec_(std::move(spec)), driver_(std::move(driver)) {
  eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::info, "demo_backend_started",
                     {{"backend", spec_.identity.name}, {"device", spec_.identity.device_id}});
}

Core::~Core() {
  eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::info, "demo_backend_stopped",
                     {{"backend", spec_.identity.name}, {"device", spec_.identity.device_id}});
}

const hardware::BackendIdentity& Core::identity() const { return spec_.identity; }

const hardware::FeedDeclarations& Core::feeds() const { return spec_.feeds; }

hardware::ReleaseResult Core::TryRelease() {
  if (halted_.load() || !driver_ || !driver_->Available(spec_.identity.device_id)) {
    eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::warn, "demo_release_rejected",
                       {{"backend", spec_.identity.name},
                        {"device", spec_.identity.device_id},
                        {"reason", "backend_unavailable"}});
    return hardware::ReleaseResult::kRejected;
  }
  return hardware::ReleaseResult::kAccepted;
}

void Core::LatchHalt() {
  if (halted_.exchange(true)) return;
  // The latch is irreversible, so this fires at most once per device per
  // process — a warn, never a periodic-loop sink.
  eos::lib::LogEvent(eos::lib::GetLogger("demo"), spdlog::level::warn, "demo_backend_halted",
                     {{"backend", spec_.identity.name}, {"device", spec_.identity.device_id}});
}

DemoHealth Core::ReadHealth() const {
  return {.alive = !halted_.load() && driver_ && driver_->Available(spec_.identity.device_id),
          .admitted = false};
}

bool Core::halted() const { return halted_.load(); }

}  // namespace eos::backends::demo

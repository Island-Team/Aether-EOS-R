#include "registry.h"

#include <exception>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::hardware_runtime {
Registry::Registry(Registry&& other) noexcept
    : devices_(std::move(other.devices_)),
      robot_(std::move(other.robot_)),
      robots_(std::move(other.robots_)),
      estop_latched_(other.estop_latched_.load()) {}

Registry& Registry::operator=(Registry&& other) noexcept {
  if (this == &other) return *this;
  devices_ = std::move(other.devices_);
  robot_ = std::move(other.robot_);
  robots_ = std::move(other.robots_);
  estop_latched_.store(other.estop_latched_.load());
  return *this;
}

void Registry::Tick(double dt_s) {
  for (Device& device : devices_) {
    try {
      device.backend().Tick(dt_s);
    } catch (const std::exception&) {
      continue;
    }
  }
}

void Registry::LatchEstop() {
  estop_latched_.store(true);
  for (Device& device : devices_) {
    try {
      device.backend().Halt();
    } catch (const std::exception& error) {
      lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "backend_safety_failed",
                    {{"device_id", device.identity().device_id},
                     {"operation", "halt"},
                     {"detail", error.what()}});
    } catch (...) {
      lib::LogEvent(lib::GetLogger("hardware"), spdlog::level::err, "backend_safety_failed",
                    {{"device_id", device.identity().device_id},
                     {"operation", "halt"},
                     {"detail", "unknown backend exception"}});
    }
  }
}

}  // namespace eos::runtime::hardware_runtime

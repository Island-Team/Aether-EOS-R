#include "registry.h"

#include <exception>
#include <vector>

#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/arm/demo_arm.h"
#include "src/eos/ext/backends/demo/base/demo_base.h"
#include "src/eos/ext/backends/demo/gimbal/demo_gimbal.h"
#include "src/eos/ext/backends/demo/gripper/demo_gripper.h"
#include "src/eos/ext/backends/demo/speaker/demo_speaker.h"
#endif
#include "src/eos/lib/log.h"

namespace eos::runtime::hardware_runtime {
namespace {

/// First backend of `kind` whose part kind string is `part_kind` and whose
/// part carries `part_id`, dynamically cast to the concrete demo `B`;
/// nullptr when absent or when the device is not that concrete backend. The
/// dynamic cast (off the polymorphic Backend base) guarantees a device built
/// by a different factory — a ros2 arm behind the same "arm" class string —
/// answers nullptr instead of undefined behavior.
template <typename B>
const B* DemoOf(const std::vector<Device>& devices, hardware::DeviceKind kind,
                const char* part_kind, const std::string& part_id) {
  for (const Device& device : devices) {
    if (device.identity().kind == kind && device.part().kind == part_kind &&
        device.part().part_id == part_id) {
      return dynamic_cast<const B*>(&device.feeds());
    }
  }
  return nullptr;
}

}  // namespace

Registry::Registry(Registry&& other) noexcept
    : devices_(std::move(other.devices_)),
      simulator_client_(std::move(other.simulator_client_)),
      robot_(std::move(other.robot_)),
      robots_(std::move(other.robots_)),
      estop_latched_(other.estop_latched_.load()) {}

Registry& Registry::operator=(Registry&& other) noexcept {
  if (this == &other) return *this;
  devices_ = std::move(other.devices_);
  simulator_client_ = std::move(other.simulator_client_);
  robot_ = std::move(other.robot_);
  robots_ = std::move(other.robots_);
  estop_latched_.store(other.estop_latched_.load());
  return *this;
}

const backends::demo::DemoBase* Registry::Base(const std::string& part_id) const {
#if defined(EOS_BACKEND_DEMO)
  return DemoOf<backends::demo::DemoBase>(devices_, hardware::DeviceKind::kBase, "base", part_id);
#else
  return nullptr;
#endif
}

const backends::demo::DemoArm* Registry::Arm(const std::string& part_id) const {
#if defined(EOS_BACKEND_DEMO)
  return DemoOf<backends::demo::DemoArm>(devices_, hardware::DeviceKind::kManipulator, "arm",
                                         part_id);
#else
  return nullptr;
#endif
}

const backends::demo::DemoGimbal* Registry::Gimbal(const std::string& part_id) const {
#if defined(EOS_BACKEND_DEMO)
  return DemoOf<backends::demo::DemoGimbal>(devices_, hardware::DeviceKind::kGimbal, "gimbal",
                                            part_id);
#else
  return nullptr;
#endif
}

const backends::demo::DemoGripper* Registry::Gripper(const std::string& part_id) const {
#if defined(EOS_BACKEND_DEMO)
  return DemoOf<backends::demo::DemoGripper>(devices_, hardware::DeviceKind::kGripper, "gripper",
                                             part_id);
#else
  return nullptr;
#endif
}

const backends::demo::DemoSpeaker* Registry::Speaker(const std::string& part_id) const {
#if defined(EOS_BACKEND_DEMO)
  return DemoOf<backends::demo::DemoSpeaker>(devices_, hardware::DeviceKind::kSpeaker, "speaker",
                                             part_id);
#else
  return nullptr;
#endif
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

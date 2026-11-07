#include "descriptor.h"

#include <chrono>
#include <memory>
#include <utility>

#include "adapters/eos-cxx/runtime/hardware/detail/config_parser.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/arm/demo_arm.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/base/demo_base.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/cam/demo_cam.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/gimbal/demo_gimbal.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/gripper/demo_gripper.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/mic/demo_mic.h"
#include "adapters/eos-cxx/runtime/hardware/backends/demo/speaker/demo_speaker.h"
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
struct EmptySettings final : BackendSettings {};
BackendDescriptor MakeSimulatorDescriptor(const std::filesystem::path& socket,
                                          SimulatorVariant variant) {
  const bool physics_backend = variant == SimulatorVariant::kMujoco;
  BackendDescriptor d;
  d.type = physics_backend ? "mujoco" : "demo";
  d.supports_class = [physics_backend](std::string_view kind) {
    return physics_backend
               ? Supports(kind, {"arm", "base", "cam", "gripper"})
               : Supports(kind, {"arm", "base", "gimbal", "cam", "mic", "gripper", "speaker"});
  };
  d.parse = [](const toml::table& tbl, const PartConfig&, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    static const char* const kAllowed[] = {"type", nullptr};
    RequireKnownKeys(tbl, kAllowed, path, where);
    return std::make_shared<EmptySettings>();
  };
  d.supports_primitive = [physics_backend](const PrimitiveSpec& spec) {
    return !physics_backend || spec.name != "arm.pick";
  };
  auto client = std::make_shared<ext::simulator::ipc::SimulatorClient>(
      socket, std::chrono::milliseconds(250),
      physics_backend ? std::chrono::seconds(30) : std::chrono::milliseconds(250));
  d.create = [client, physics_backend](
                 std::string id, const PartConfig& part,
                 const BackendSettings&) -> std::unique_ptr<hardware::Backend> {
    if (part.kind == "arm") {
      const auto a = part.arm.value_or(ArmPartConfig{});
      return std::make_unique<demo::DemoArm>(
          std::move(id), client, a.link_lengths_m[0].value_or(0.0),
          a.link_lengths_m[1].value_or(0.0), a.mount_x_m, a.mount_y_m,
          part.joint_names.empty() ? 6 : part.joint_names.size(), physics_backend);
    }
    if (part.kind == "base") {
      const auto b = part.base.value_or(BasePartConfig{});
      return std::make_unique<demo::DemoBase>(std::move(id), client, b.footprint_width_m,
                                              b.footprint_length_m);
    }
    if (part.kind == "gimbal") return std::make_unique<demo::DemoGimbal>(std::move(id), client);
    if (part.kind == "cam") return std::make_unique<demo::DemoCam>(std::move(id), client);
    if (part.kind == "mic") return std::make_unique<demo::DemoMic>(std::move(id), client);
    if (part.kind == "gripper") return std::make_unique<demo::DemoGripper>(std::move(id), client);
    if (part.kind == "speaker") return std::make_unique<demo::DemoSpeaker>(std::move(id), client);
    return nullptr;
  };
  return d;
}

}  // namespace eos::backends

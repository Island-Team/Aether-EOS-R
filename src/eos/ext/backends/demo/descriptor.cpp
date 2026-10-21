#include "descriptor.h"

#include <memory>
#include <utility>

#include "src/eos/ext/backends/detail/config_parser.h"
#if defined(EOS_BACKEND_DEMO)
#include "src/eos/ext/backends/demo/arm/demo_arm.h"
#include "src/eos/ext/backends/demo/base/demo_base.h"
#include "src/eos/ext/backends/demo/cam/demo_cam.h"
#include "src/eos/ext/backends/demo/gimbal/demo_gimbal.h"
#include "src/eos/ext/backends/demo/gripper/demo_gripper.h"
#include "src/eos/ext/backends/demo/mic/demo_mic.h"
#include "src/eos/ext/backends/demo/speaker/demo_speaker.h"
#endif
namespace eos::backends {
using namespace eos::runtime::hardware_runtime;
using namespace eos::backends::detail;
struct EmptySettings final : BackendSettings {};
BackendDescriptor DemoDescriptor(const std::filesystem::path& socket) {
  BackendDescriptor d;
  d.type = "demo";
#if !defined(EOS_BACKEND_DEMO)
  d.compiled = false;
#endif
  d.supports_class = [](std::string_view kind) {
    return Supports(kind, {"arm", "base", "gimbal", "cam", "mic", "gripper", "speaker"});
  };
  d.parse = [](const toml::table& tbl, const PartConfig&, const std::string& path,
               const std::string& where) -> std::shared_ptr<const BackendSettings> {
    static const char* const kAllowed[] = {"type", nullptr};
    RequireKnownKeys(tbl, kAllowed, path, where);
    return std::make_shared<EmptySettings>();
  };
  d.supports_primitive = [](const PrimitiveSpec&) { return true; };
#if defined(EOS_BACKEND_DEMO)
  auto client = std::make_shared<ext::simulator::ipc::SimulatorClient>(socket);
  d.create = [client](std::string id, const PartConfig& part,
                      const BackendSettings&) -> std::unique_ptr<hardware::Backend> {
    if (part.kind == "arm") {
      const auto a = part.arm.value_or(ArmPartConfig{});
      return std::make_unique<demo::DemoArm>(
          std::move(id), client, a.link_lengths_m[0].value_or(0.0),
          a.link_lengths_m[1].value_or(0.0), a.mount_x_m, a.mount_y_m,
          part.joint_names.empty() ? 6 : part.joint_names.size());
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
#else
  (void)socket;
  d.create = [](std::string, const PartConfig&,
                const BackendSettings&) -> std::unique_ptr<hardware::Backend> { return nullptr; };
#endif
  return d;
}

}  // namespace eos::backends

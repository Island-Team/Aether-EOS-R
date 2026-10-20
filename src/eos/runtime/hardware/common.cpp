#include "class.h"

namespace eos::runtime::hardware_runtime::detail {
std::unique_ptr<hardware::Backend> MakeBackend(
    const std::string& type, std::string device_id, const PartConfig& part,
    std::shared_ptr<ext::simulator::ipc::SimulatorClient> simulator_world) {
  if (part.kind == "arm") return MakeArmBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "base")
    return MakeBaseBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "cam")
    return MakeCameraBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "gimbal")
    return MakeGimbalBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "gripper")
    return MakeGripperBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "mic") return MakeMicBackend(type, std::move(device_id), part, simulator_world);
  if (part.kind == "speaker")
    return MakeSpeakerBackend(type, std::move(device_id), part, simulator_world);
  return nullptr;
}
}  // namespace eos::runtime::hardware_runtime::detail

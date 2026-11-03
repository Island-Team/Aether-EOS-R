#include "descriptor.h"

#include "src/eos/runtime/hardware/backends/simulator/descriptor.h"

namespace eos::backends {

runtime::hardware_runtime::BackendDescriptor MujocoDescriptor(const std::filesystem::path& socket) {
  return MakeSimulatorDescriptor(socket, SimulatorVariant::kMujoco);
}

}  // namespace eos::backends

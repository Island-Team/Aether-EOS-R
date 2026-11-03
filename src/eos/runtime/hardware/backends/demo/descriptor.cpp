#include "descriptor.h"

#include "src/eos/runtime/hardware/backends/simulator/descriptor.h"

namespace eos::backends {

runtime::hardware_runtime::BackendDescriptor DemoDescriptor(const std::filesystem::path& socket) {
  return MakeSimulatorDescriptor(socket, SimulatorVariant::kDemo);
}

}  // namespace eos::backends

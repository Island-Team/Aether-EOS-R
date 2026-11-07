#include "descriptor.h"

#include "adapters/eos-cxx/runtime/hardware/backends/simulator/descriptor.h"

namespace eos::backends {

runtime::hardware_runtime::BackendDescriptor DemoDescriptor(const std::filesystem::path& socket) {
  return MakeSimulatorDescriptor(socket, SimulatorVariant::kDemo);
}

}  // namespace eos::backends

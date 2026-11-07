#pragma once

#include <filesystem>

#include "adapters/eos-cxx/runtime/hardware/catalog.h"

namespace eos::backends {

enum class SimulatorVariant { kDemo, kMujoco };

runtime::hardware_runtime::BackendDescriptor MakeSimulatorDescriptor(
    const std::filesystem::path& socket, SimulatorVariant variant);

}  // namespace eos::backends

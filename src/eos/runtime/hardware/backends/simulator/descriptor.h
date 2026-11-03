#pragma once

#include <filesystem>

#include "src/eos/runtime/hardware/catalog.h"

namespace eos::backends {

enum class SimulatorVariant { kDemo, kMujoco };

runtime::hardware_runtime::BackendDescriptor MakeSimulatorDescriptor(
    const std::filesystem::path& socket, SimulatorVariant variant);

}  // namespace eos::backends

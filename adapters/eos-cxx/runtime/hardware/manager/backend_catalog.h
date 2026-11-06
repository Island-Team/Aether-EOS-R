#pragma once
#include <filesystem>

#include "adapters/eos-cxx/runtime/hardware/catalog.h"

namespace eos::runtime::hardware_runtime::manager {
BackendCatalog BuildBackendCatalog(
    const std::filesystem::path& simulator_socket = "var/run/simulator.sock");
}  // namespace eos::runtime::hardware_runtime::manager

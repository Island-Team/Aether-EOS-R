#pragma once
#include <filesystem>

#include "src/eos/runtime/hardware/catalog.h"
namespace eos::backends {
runtime::hardware_runtime::BackendCatalog BuildBuiltinBackendCatalog(
    const std::filesystem::path& demo_socket = "var/run/simulator.sock");
}

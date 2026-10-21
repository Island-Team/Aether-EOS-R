#pragma once
#include <filesystem>

#include "src/eos/runtime/hardware/catalog.h"
namespace eos::backends {
runtime::hardware_runtime::BackendDescriptor DemoDescriptor(
    const std::filesystem::path& socket = "var/run/simulator.sock");
}

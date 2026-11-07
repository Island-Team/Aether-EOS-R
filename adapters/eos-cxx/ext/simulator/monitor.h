/// JSON serialization for the standalone simulator service.
#pragma once

#include <nlohmann/json.hpp>

#include "simulator.h"

namespace eos::ext::simulator {

/// Canonical v2 snapshot used by the standalone simulator WebSocket surface.
nlohmann::json SimulatorJson(const SimulatorSnapshot& snapshot);

}  // namespace eos::ext::simulator

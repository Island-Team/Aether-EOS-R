/// Immutable read models published by the standalone EOS simulator.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "raster_map.h"
#include "scene.h"
#include "world.h"

namespace eos::ext::simulator {

/// Atomic read-only projection consumed by the simulator WebSocket API.
struct SimulatorSnapshot {
  std::uint64_t state_sequence{0};
  RasterMapMetadata map;
  Scene scene;
  BaseState base;
  GimbalState gimbal;
  double camera_max_range_m{100.0};
  std::optional<LineHit> camera_hit;
  ArmState arm;
  GripperState gripper;
  SpeakerState speaker;
};

/// Cached raster response paired with one simulator map revision.
struct SimulatorRaster {
  std::string map_id;
  std::uint64_t revision{0};
  std::vector<std::uint8_t> png;
};

}  // namespace eos::ext::simulator

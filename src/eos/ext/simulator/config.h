#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace eos::ext::simulator {

inline constexpr std::uint16_t kSimulatorPort = 16004;

enum class CameraSourceMode {
  kScene,
  kSystem,
};

enum class SimulatorBackend { kDemo, kMujoco };

struct MujocoConfig {
  std::filesystem::path executable;
  std::filesystem::path scene;
};

struct SimulatorCameraConfig {
  std::uint32_t width_px{1920};
  std::uint32_t height_px{1080};
  std::uint32_t fps{30};
};

struct SimulationConfig {
  SimulatorBackend backend{SimulatorBackend::kDemo};
  std::string host{"127.0.0.1"};
  std::uint16_t port{kSimulatorPort};
  std::string map_id;
  CameraSourceMode camera_source{CameraSourceMode::kScene};
  SimulatorCameraConfig camera;
  MujocoConfig mujoco;
};

/// Loads standalone simulator configuration. The active map is selected by
/// stable id under `<state>/simulator/maps` and changes require a restart.
SimulationConfig LoadSimulationConfig(const std::string& path);

}  // namespace eos::ext::simulator

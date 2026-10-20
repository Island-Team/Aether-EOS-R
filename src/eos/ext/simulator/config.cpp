#include "src/eos/ext/simulator/config.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <toml++/toml.hpp>

#include "src/eos/ext/simulator/map_store.h"
namespace eos::ext::simulator {
namespace {
[[noreturn]] void Fail(const std::string& path, const std::string& message) {
  throw std::runtime_error("simulation config '" + path + "': " + message);
}
}  // namespace
SimulationConfig LoadSimulationConfig(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) Fail(path, "cannot open file for reading");
  std::ostringstream contents;
  contents << input.rdbuf();
  toml::table root;
  try {
    root = toml::parse(contents.str(), path);
  } catch (const toml::parse_error& error) {
    Fail(path, std::string(error.description()));
  }
  for (const auto& [key, value] : root) {
    (void)value;
    if (key != "simulator") Fail(path, "unknown root key '" + std::string(key.str()) + "'");
  }
  const auto* simulator = root["simulator"].as_table();
  if (!simulator) Fail(path, "[simulator] must be a table");
  for (const auto& [key, value] : *simulator) {
    (void)value;
    if (key != "host" && key != "port" && key != "map_id" && key != "camera_source" &&
        key != "camera")
      Fail(path, "[simulator] has unknown key '" + std::string(key.str()) + "'");
  }
  SimulationConfig config;
  auto host = (*simulator)["host"].value<std::string>();
  if (!host || host->empty()) Fail(path, "[simulator] host must be a non-empty string");
  config.host = *host;
  auto port = (*simulator)["port"].value<std::int64_t>();
  if (!port || *port != kSimulatorPort) Fail(path, "[simulator] port must be 16004");
  config.port = static_cast<std::uint16_t>(*port);
  auto map_id = (*simulator)["map_id"].value<std::string>();
  if (!map_id) Fail(path, "[simulator] map_id must be a string");
  try {
    if (!map_id->empty()) MapStore::ValidateId(*map_id);
  } catch (const std::exception& error) {
    Fail(path, error.what());
  }
  config.map_id = *map_id;
  const auto camera_source = (*simulator)["camera_source"].value<std::string>();
  if (!camera_source) Fail(path, "[simulator] camera_source must be a string");
  if (*camera_source == "scene") {
    config.camera_source = CameraSourceMode::kScene;
  } else if (*camera_source == "system") {
    config.camera_source = CameraSourceMode::kSystem;
  } else {
    Fail(path, "[simulator] camera_source must be 'scene' or 'system'");
  }
  const auto* camera = (*simulator)["camera"].as_table();
  if (!camera) Fail(path, "[simulator.camera] must be a table");
  for (const auto& [key, value] : *camera) {
    (void)value;
    if (key != "width_px" && key != "height_px" && key != "fps")
      Fail(path, "[simulator.camera] has unknown key '" + std::string(key.str()) + "'");
  }
  const auto read_bounded = [&](const char* key, std::int64_t maximum) {
    const auto value = (*camera)[key].value<std::int64_t>();
    if (!value || *value < 1 || *value > maximum)
      Fail(path, "[simulator.camera] " + std::string(key) + " must be an integer from 1 to " +
                     std::to_string(maximum));
    return static_cast<std::uint32_t>(*value);
  };
  config.camera.width_px = read_bounded("width_px", 16384);
  config.camera.height_px = read_bounded("height_px", 16384);
  config.camera.fps = read_bounded("fps", 240);
  return config;
}
}  // namespace eos::ext::simulator

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "src/eos/ext/simulator/config.h"
#include "src/eos/ext/simulator/ipc/server.h"
#include "src/eos/ext/simulator/map_store.h"
#include "src/eos/ext/simulator/raster_map.h"
#include "src/eos/ext/simulator/scene.h"
#include "src/eos/ext/simulator/system_camera_source.h"
#include "src/eos/ext/simulator/web_server.h"
#include "src/eos/ext/simulator/world.h"
#include "src/eos/lib/log.h"
#include "src/eos/lib/paths.h"

namespace {
std::atomic_bool stop{false};
void Signal(int) { stop = true; }
struct Options {
  std::string config, etc, contracts;
  std::optional<std::uint16_t> port;
  bool help = false;
};
Options Parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--config" && i + 1 < argc)
      o.config = argv[++i];
    else if (a == "--etc" && i + 1 < argc)
      o.etc = argv[++i];
    else if (a == "--contracts" && i + 1 < argc)
      o.contracts = argv[++i];
    else if (a == "--port" && i + 1 < argc) {
      const std::string value = argv[++i];
      std::size_t consumed = 0;
      const unsigned long parsed = std::stoul(value, &consumed);
      if (consumed != value.size() || parsed == 0 || parsed > 65535)
        throw std::invalid_argument("--port must be an integer from 1 to 65535");
      o.port = static_cast<std::uint16_t>(parsed);
    } else if (a == "--help" || a == "-h")
      o.help = true;
    else
      throw std::invalid_argument(
          "usage: eos-simulator [--config PATH] [--etc PATH] [--contracts PATH] [--port N]");
  }
  return o;
}
}  // namespace
int main(int argc, char** argv) {
  std::signal(SIGINT, Signal);
  std::signal(SIGTERM, Signal);
  try {
    auto options = Parse(argc, argv);
    if (options.help) {
      std::fprintf(stderr,
                   "usage: eos-simulator [--config PATH] [--etc PATH] [--contracts PATH] "
                   "[--port N]\n");
      return 0;
    }
    auto paths = eos::lib::ResolveLayoutPaths(options.etc, options.contracts);
    auto config_path =
        options.config.empty() ? paths.etc / "sim.toml" : std::filesystem::path(options.config);
    if (!config_path.is_absolute()) throw std::invalid_argument("config path must be absolute");
    auto config = eos::ext::simulator::LoadSimulationConfig(config_path.string());
    if (options.port) config.port = *options.port;
    eos::lib::ConfigureLogging(paths.log, "simulator", eos::lib::LogConfig{});
    auto logger = eos::lib::GetLogger("simulator");
    eos::ext::simulator::World world;
    std::shared_ptr<eos::ext::simulator::SystemCameraSource> system_camera;
    if (config.camera_source == eos::ext::simulator::CameraSourceMode::kSystem) {
      system_camera = std::make_shared<eos::ext::simulator::SystemCameraSource>(config.camera);
      world.SetCameraSource(system_camera);
    }
    eos::ext::simulator::MapStore maps(paths.state);
    eos::ext::simulator::SourceLibrary sources(paths.state);
    auto reload = [&] {
      if (config.map_id.empty()) return;
      auto bundle = maps.root() / config.map_id;
      auto manifest = maps.ReadMetadata(config.map_id);
      world.SetSimulator(eos::ext::simulator::RasterMap::LoadBundle(bundle),
                         eos::ext::simulator::ParseScene({{"version", 2},
                                                          {"revision", manifest.at("revision")},
                                                          {"lines", manifest.at("lines")}}),
                         paths.state);
      auto pose = manifest.at("initial_base_pose");
      world.SetBasePose(pose.at("x_m"), pose.at("y_m"), pose.at("yaw_rad"));
      eos::lib::LogEvent(logger, spdlog::level::info, "simulator_map_loaded",
                         {{"map_id", config.map_id}, {"revision", manifest.at("revision")}});
    };
    reload();
    eos::ext::simulator::ipc::SimulatorService rpc(world);
    eos::ext::simulator::WebServer web(std::move(maps), std::move(sources), world, config.map_id,
                                       reload);
    std::thread rpc_thread([&] { rpc.Serve(paths.run / "simulator.sock", stop); });
    std::thread tick_thread([&] {
      auto before = std::chrono::steady_clock::now();
      while (!stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = std::chrono::steady_clock::now();
        world.Tick(std::chrono::duration<double>(now - before).count());
        before = now;
      }
    });
    eos::lib::LogEvent(
        logger, spdlog::level::info, "simulator_started",
        {{"map_id", config.map_id},
         {"camera_source", config.camera_source == eos::ext::simulator::CameraSourceMode::kSystem
                               ? "system"
                               : "scene"}});
    try {
      web.Serve(config.host, config.port, stop);
    } catch (...) {
      stop = true;
      rpc_thread.join();
      tick_thread.join();
      throw;
    }
    stop = true;
    rpc_thread.join();
    tick_thread.join();
    eos::lib::LogEvent(logger, spdlog::level::info, "simulator_stopped");
    return 0;
  } catch (const std::exception& e) {
    try {
      eos::lib::ConfigureLogging(eos::lib::DefaultLogDirectory(), "simulator",
                                 eos::lib::LogConfig{});
      eos::lib::LogEvent(eos::lib::GetLogger("simulator"), spdlog::level::err,
                         "simulator_start_failed", {{"reason", e.what()}});
    } catch (...) {
      // Startup still fails cleanly when even the default file sink is unavailable.
    }
    std::fprintf(stderr, "eos-simulator: %s\n", e.what());
    return 1;
  }
}

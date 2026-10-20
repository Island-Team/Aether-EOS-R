#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

#include "src/eos/ext/simulator/ipc/client.h"
#include "src/eos/ext/simulator/ipc/server.h"
#include "src/eos/ext/simulator/world.h"
#include "src/eos/lib/uuid.h"

namespace eos::test {

class InProcessSimulator {
 public:
  InProcessSimulator()
      : root_(std::filesystem::temp_directory_path() / ("eos-sim-" + lib::MintUuidV7())),
        socket_path_(root_ / "simulator.sock"),
        service_(world_) {
    std::filesystem::create_directories(root_);
    server_ = std::thread([this] {
      try {
        service_.Serve(socket_path_, stop_);
      } catch (const std::exception& error) {
        server_error_ = error.what();
        server_failed_.store(true);
      } catch (...) {
        server_error_ = "unknown simulator server failure";
        server_failed_.store(true);
      }
    });

    ext::simulator::ipc::SimulatorClient client(socket_path_, std::chrono::milliseconds(50));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !server_failed_.load()) {
      if (client.Available("base-0")) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Shutdown();
    if (server_failed_.load()) {
      throw std::runtime_error("in-process simulator failed to start: " + server_error_);
    }
    throw std::runtime_error("in-process simulator was not ready within 5 seconds");
  }

  ~InProcessSimulator() { Shutdown(); }

  InProcessSimulator(const InProcessSimulator&) = delete;
  InProcessSimulator& operator=(const InProcessSimulator&) = delete;

  const std::filesystem::path& socket_path() const { return socket_path_; }

 private:
  void Shutdown() {
    stop_.store(true);
    if (server_.joinable()) server_.join();
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  std::filesystem::path root_;
  std::filesystem::path socket_path_;
  ext::simulator::World world_;
  ext::simulator::ipc::SimulatorService service_;
  std::atomic_bool stop_{false};
  std::atomic_bool server_failed_{false};
  std::string server_error_;
  std::thread server_;
};

}  // namespace eos::test

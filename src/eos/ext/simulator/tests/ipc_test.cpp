#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "../ipc/client.h"
#include "../ipc/server.h"
#include "../world.h"

namespace eos::ext::simulator::ipc {
namespace {
using namespace std::chrono_literals;

TEST(SimulatorIpcTest, RecoversAcrossExternalServiceLifecycle) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("eos-simulator-ipc-" + std::to_string(
                                                std::chrono::steady_clock::now()
                                                    .time_since_epoch()
                                                    .count()));
  const auto socket = root / "simulator.sock";
  std::filesystem::create_directories(root);
  SimulatorClient client(socket, 50ms);
  EXPECT_FALSE(client.Available("base-0"));

  World world;
  SimulatorService service(world);
  std::atomic_bool stop{false};
  std::thread server([&] { service.Serve(socket, stop); });
  for (int attempt = 0; attempt < 100 && !client.Available("base-0"); ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(client.Available("base-0"));
  ASSERT_TRUE(client.SetBasePosture("base-0", 2));
  const auto state = client.ReadBase("base-0");
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(state->posture, 2);

  stop = true;
  server.join();
  EXPECT_FALSE(client.Available("base-0"));
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace eos::ext::simulator::ipc

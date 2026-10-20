#include "src/eos/runtime/init/config.h"
#include "src/eos/runtime/init/prelaunch.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace eos::runtime::init {
namespace {

TEST(ManagedAdapterConfigTest, ParsesAndValidatesManagedAdapter) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("eos-adapter-config-" + std::to_string(getpid()) + ".toml");
  {
    std::ofstream file(path);
    file << "[launch.adapter]\n"
            "shell='exec sleep 60'\n"
            "ready_shell='true'\n"
            "timeout_ms=250\n"
            "reuse_if_ready=true\n"
            "[[launch.prelaunch]]\n"
            "shell='exec false'\n"
            "ready_shell='true'\n"
            "reuse_if_ready=true\n"
            "restart_on_exit=true\n"
            "[mode]\n[[mode.binding]]\nbackend='demo'\nprofile='demo'\n";
  }
  const Config config = LoadConfig(path.string());
  EXPECT_EQ(config.adapter.shell, "exec sleep 60");
  EXPECT_EQ(config.adapter.ready_shell, "true");
  EXPECT_EQ(config.adapter.timeout_ms, 250u);
  EXPECT_TRUE(config.adapter.reuse_if_ready);
  ASSERT_EQ(config.prelaunch.size(), 1u);
  EXPECT_TRUE(config.prelaunch[0].reuse_if_ready);
  EXPECT_TRUE(config.prelaunch[0].restart_on_exit);
  EXPECT_EQ(config.prelaunch[0].ready_shell, "true");
  {
    std::ofstream file(path);
    file << "[launch.adapter]\nshell='exec sleep 60'\n"
            "[mode]\n[[mode.binding]]\nbackend='demo'\nprofile='demo'\n";
  }
  EXPECT_THROW(LoadConfig(path.string()), std::runtime_error);
  {
    std::ofstream file(path);
    file << "[launch]\n[[launch.prelaunch]]\nshell='exec false'\n"
            "reuse_if_ready=true\n"
            "[mode]\n[[mode.binding]]\nbackend='demo'\nprofile='demo'\n";
  }
  EXPECT_THROW(LoadConfig(path.string()), std::runtime_error);
  {
    std::ofstream file(path);
    file << "[launch]\n[[launch.prelaunch]]\ncommand=['/bin/true']\n"
            "restart_on_exit=true\n"
            "[mode]\n[[mode.binding]]\nbackend='demo'\nprofile='demo'\n";
  }
  EXPECT_THROW(LoadConfig(path.string()), std::runtime_error);
  std::filesystem::remove(path);
}

TEST(PrelaunchSupervisorTest, RestartsExitedShellAndStopsWithOwner) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-prelaunch-restart-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto marker = directory / "starts.txt";
  {
    auto processes = RunPrelaunch(
        {{.shell = "printf 'started\\n' >> '" + marker.string() + "'; exit 1",
          .restart_on_exit = true}},
        directory.string());
    bool restarted = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
      std::ifstream lines(marker);
      const auto count = std::count(std::istreambuf_iterator<char>(lines),
                                    std::istreambuf_iterator<char>(), '\n');
      if (count >= 2) {
        restarted = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(restarted);
  }
  ASSERT_TRUE(std::filesystem::exists(marker));
  const auto size_after_stop = std::filesystem::file_size(marker);
  std::this_thread::sleep_for(std::chrono::milliseconds(2300));
  EXPECT_EQ(std::filesystem::file_size(marker), size_after_stop);
  std::filesystem::remove_all(directory);
}

TEST(PrelaunchSupervisorTest, RelaunchesAfterReusedServiceDisappears) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-prelaunch-reuse-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto ready = directory / "ready";
  const auto marker = directory / "started";
  std::ofstream(ready).close();
  {
    auto processes = RunPrelaunch(
        {{.shell = "touch '" + marker.string() + "'; exec sleep 60",
          .ready_shell = "test -f '" + ready.string() + "'",
          .reuse_if_ready = true,
          .restart_on_exit = true}},
        directory.string());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_FALSE(std::filesystem::exists(marker));
    std::filesystem::remove(ready);
    bool started = false;
    for (int attempt = 0; attempt < 60; ++attempt) {
      if (std::filesystem::exists(marker)) {
        started = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(started);
  }
  std::filesystem::remove_all(directory);
}

TEST(ManagedAdapterTest, RequiresReadinessAndStopsOwnedService) {
  const auto log_dir = std::filesystem::temp_directory_path() /
                       ("eos-adapter-logs-" + std::to_string(getpid()));
  std::filesystem::create_directories(log_dir);
  EXPECT_NO_THROW({
    auto service = RunManagedService(
        {.shell = "exec sleep 60", .ready_shell = "true", .timeout_ms = 2000},
        log_dir.string());
  });
  EXPECT_THROW(RunManagedService(
                   {.shell = "exec false", .ready_shell = "false", .timeout_ms = 1000},
                   log_dir.string()),
               std::runtime_error);
  EXPECT_THROW(RunManagedService(
                   {.shell = "exec false", .ready_shell = "true", .timeout_ms = 1000},
                   log_dir.string()),
               std::runtime_error);
  EXPECT_NO_THROW({
    auto reused = RunManagedService(
        {.shell = "exec false", .ready_shell = "true", .reuse_if_ready = true},
        log_dir.string());
  });
  EXPECT_NO_THROW({
    auto reused = RunPrelaunch(
        {{.shell = "exec false", .ready_shell = "true", .reuse_if_ready = true}},
        log_dir.string());
  });
  std::filesystem::remove_all(log_dir);
}

}  // namespace
}  // namespace eos::runtime::init

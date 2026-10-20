#include "src/eos/ext/simulator/config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace eos::ext::simulator {
namespace {

class SimulationConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = std::filesystem::temp_directory_path() /
            ("eos-simulation-config-test-" + std::to_string(++sequence_) + ".toml");
  }
  void TearDown() override { std::filesystem::remove(path_); }
  void Write(const std::string& text) {
    std::ofstream output(path_);
    ASSERT_TRUE(output);
    output << text;
  }

  static inline unsigned sequence_{0};
  std::filesystem::path path_;
};

TEST_F(SimulationConfigTest, LoadsCommittedConfiguration) {
  const SimulationConfig config = LoadSimulationConfig(std::string(EOS_SOURCE_ETC) + "/sim.toml");
  EXPECT_EQ(config.host, "127.0.0.1");
  EXPECT_EQ(config.port, 16004);
  EXPECT_TRUE(config.map_id.empty());
  EXPECT_EQ(config.camera_source, CameraSourceMode::kScene);
  EXPECT_EQ(config.camera.width_px, 1920u);
  EXPECT_EQ(config.camera.height_px, 1080u);
  EXPECT_EQ(config.camera.fps, 30u);
}

TEST_F(SimulationConfigTest, LoadsConfiguredMapId) {
  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id='lab'\ncamera_source='scene'\n"
      "[simulator.camera]\nwidth_px=640\nheight_px=480\nfps=15\n");
  const SimulationConfig config = LoadSimulationConfig(path_.string());
  EXPECT_EQ(config.map_id, "lab");
  EXPECT_EQ(config.camera_source, CameraSourceMode::kScene);
  EXPECT_EQ(config.camera.width_px, 640u);
}

TEST_F(SimulationConfigTest, AcceptsOptInSystemCamera) {
  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='system'\n"
      "[simulator.camera]\nwidth_px=640\nheight_px=480\nfps=15\n");
  EXPECT_EQ(LoadSimulationConfig(path_.string()).camera_source, CameraSourceMode::kSystem);
}

TEST_F(SimulationConfigTest, RejectsAnyOtherPort) {
  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16005\nmap_id=''\ncamera_source='scene'\n[simulator."
      "camera]\nwidth_px=1\nheight_px=1\nfps=1\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);
}

TEST_F(SimulationConfigTest, RejectsMissingAndUnknownSettings) {
  Write("[simulator]\nport=16004\nmap_id=''\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='scene'\nextra=true\n["
      "simulator.camera]\nwidth_px=1\nheight_px=1\nfps=1\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=[]\ncamera_source='scene'\n[simulator."
      "camera]\nwidth_px=1\nheight_px=1\nfps=1\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='other'\n[simulator."
      "camera]\nwidth_px=1\nheight_px=1\nfps=1\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write("[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='scene'\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='scene'\n"
      "[simulator.camera]\nwidth_px=0\nheight_px=1080\nfps=30\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);

  Write(
      "[simulator]\nhost='127.0.0.1'\nport=16004\nmap_id=''\ncamera_source='scene'\n"
      "[simulator.camera]\nwidth_px=1920\nheight_px=1080\nfps=30\nextra=true\n");
  EXPECT_THROW(LoadSimulationConfig(path_.string()), std::runtime_error);
}

}  // namespace
}  // namespace eos::ext::simulator

/// Backend-local parsing tests through the same catalog used at startup.
#include <gtest/gtest.h>

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

#include "src/eos/runtime/hardware/manager/backend_catalog.h"
#include "src/eos/runtime/hardware/binding.h"

namespace {

using eos::runtime::hardware_runtime::ParseBinding;

std::string Device(std::string_view kind, std::string_view type, std::string_view settings = "") {
  std::string text = "[[profile.test.device]]\nclass = \"" + std::string(kind) +
                     "\"\n[profile.test.device.backend]\ntype = \"" + std::string(type) + "\"\n";
  text += settings;
  text += "\n[profile.test.device.feeds.health]\nmax_rate_hz = 10\n";
  if (kind == "cam" || kind == "mic") {
    text += "[profile.test.device.feeds.sensor]\nmax_rate_hz = 250\n";
  } else {
    text += "[profile.test.device.feeds.proprioception]\nmax_rate_hz = 500\n";
  }
  return text;
}

std::string ParseError(std::string_view body) {
  try {
    ParseBinding(body, "backend-test.toml", "test", eos::runtime::hardware_runtime::manager::BuildBackendCatalog());
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return "";
}

TEST(BackendConfigTest, DemoRejectsUnexpectedSettings) {
  EXPECT_TRUE(ParseError(Device("base", "demo")).empty());
  EXPECT_NE(ParseError(Device("base", "demo", "[profile.test.device.backend.demo]\nextra = true\n"))
                .find("unknown key"),
            std::string::npos);
}

TEST(BackendConfigTest, AlsaCaptureValidatesClassAndWindow) {
  const char* table = "[profile.test.device.backend.alsa]\nalsa_device = \"fixture\"\n";
  EXPECT_TRUE(ParseError(Device("mic", "alsa", table)).empty());
  EXPECT_NE(ParseError(Device("speaker", "alsa", table)).find("does not implement class 'speaker'"),
            std::string::npos);
  EXPECT_NE(
      ParseError(Device("mic", "alsa", "[profile.test.device.backend.alsa]\nwindow_frames = 0\n"))
          .find("window_frames"),
      std::string::npos);
}

TEST(BackendConfigTest, AplayValidatesPlaybackLimit) {
  EXPECT_TRUE(ParseError(Device("speaker", "aplay",
                                "[profile.test.device.backend.aplay]\nalsa_device = \"fixture\"\n"))
                  .empty());
  EXPECT_NE(ParseError(Device("speaker", "aplay",
                              "[profile.test.device.backend.aplay]\nmax_stream_bytes = 0\n"))
                .find("max_stream_bytes"),
            std::string::npos);
}

TEST(BackendConfigTest, V4l2ValidatesCaptureDimensions) {
  EXPECT_TRUE(
      ParseError(Device("cam", "v4l2",
                        "[profile.test.device.backend.v4l2]\nvideo_device = \"/dev/video-test\"\n"))
          .empty());
  EXPECT_NE(ParseError(Device("cam", "v4l2", "[profile.test.device.backend.v4l2]\nwidth_px = 0\n"))
                .find("width_px"),
            std::string::npos);
}

TEST(BackendConfigTest, HostBackendsRejectUnknownKeys) {
  EXPECT_NE(ParseError(Device("mic", "alsa", "[profile.test.device.backend.alsa]\nextra = true\n"))
                .find("unknown key 'extra'"),
            std::string::npos);
  EXPECT_NE(
      ParseError(Device("speaker", "aplay", "[profile.test.device.backend.aplay]\nextra = true\n"))
          .find("unknown key 'extra'"),
      std::string::npos);
  EXPECT_NE(ParseError(Device("cam", "v4l2", "[profile.test.device.backend.v4l2]\nextra = true\n"))
                .find("unknown key 'extra'"),
            std::string::npos);
}

TEST(BackendConfigTest, Ros2RejectsWrongEndpointMessage) {
  const std::string config = R"toml([profile.test.device.backend.ros2]
domain_id = 0
namespace = "/test"
[profile.test.device.backend.ros2.image]
kind = "topic"
name = "/test/image"
message = "std_msgs/String"
)toml";
  const std::string error = ParseError(Device("cam", "ros2", config));
  EXPECT_NE(error.find("image"), std::string::npos);
  EXPECT_NE(error.find("sensor_msgs/CompressedImage"), std::string::npos);
}

TEST(BackendConfigTest, Ros2RequiresEndpointTable) {
  EXPECT_NE(ParseError(Device("arm", "ros2")).find("device.backend.ros2"), std::string::npos);
  EXPECT_NE(ParseError(Device("speaker", "ros2")).find("class"), std::string::npos);
}

TEST(BackendConfigTest, Ros2CameraKeepsUnknownFovAbsent) {
  const auto parse_camera = [](bool explicit_fov) {
    std::string text = "[[profile.test.device]]\nclass = \"cam\"\n";
    if (explicit_fov) {
      text += "[profile.test.device.part]\nfov_rad = 1.2\n";
    }
    text += R"toml([profile.test.device.backend]
type = "ros2"
[profile.test.device.backend.ros2]
domain_id = 0
namespace = "/test"
[profile.test.device.backend.ros2.image]
kind = "topic"
name = "/test/image"
message = "sensor_msgs/CompressedImage"
[profile.test.device.feeds.sensor]
max_rate_hz = 30
[profile.test.device.feeds.health]
max_rate_hz = 10
)toml";
    return ParseBinding(text, "ros2-camera.toml", "test",
                        eos::runtime::hardware_runtime::manager::BuildBackendCatalog());
  };
  const auto unknown = parse_camera(false);
  ASSERT_EQ(unknown.devices.size(), 1u);
  ASSERT_TRUE(unknown.devices[0].part.camera.has_value());
  EXPECT_FALSE(unknown.devices[0].part.camera->fov_rad.has_value());
  const auto configured = parse_camera(true);
  ASSERT_EQ(configured.devices.size(), 1u);
  ASSERT_TRUE(configured.devices[0].part.camera.has_value());
  ASSERT_TRUE(configured.devices[0].part.camera->fov_rad.has_value());
  EXPECT_DOUBLE_EQ(*configured.devices[0].part.camera->fov_rad, 1.2);
}

TEST(BackendConfigTest, VendorRequiresExplicitNetworkConfiguration) {
  EXPECT_NE(ParseError(Device("base", "vendor")).find("device.backend.vendor"), std::string::npos);
  EXPECT_NE(ParseError(Device("arm", "vendor")).find("class"), std::string::npos);
}

TEST(BackendConfigTest, VendorObstacleEndpointIsOptionalAndValidated) {
  const std::string vendor = R"toml([profile.test.device.backend.vendor]
motion_host = "127.0.0.1"
motion_port = 43893
status_bind_host = "127.0.0.1"
status_port = 43897
expected_sender = "127.0.0.1"
robotserver_host = "127.0.0.1"
robotserver_port = 30000
io_timeout_ms = 5000
navigation_timeout_ms = 300000
transition_timeout_ms = 15000
idle_stand_ms = 1000
map_id = 0
)toml";
  EXPECT_TRUE(ParseError(Device("base", "vendor", vendor)).empty());
  EXPECT_TRUE(ParseError(Device("base", "vendor", vendor +
                                "obstacle_host = \"127.0.0.1\"\nobstacle_port = 19105\n"
                                "obstacle_timeout_ms = 500\nobstacle_max_age_ms = 500\n"))
                  .empty());
  EXPECT_NE(ParseError(Device("base", "vendor", vendor + "obstacle_port = 19105\n"))
                .find("obstacle_host"), std::string::npos);
  EXPECT_NE(ParseError(Device("base", "vendor", vendor +
                                "obstacle_host = \"collector.local\"\n"
                                "obstacle_port = 19105\nobstacle_timeout_ms = 500\n"
                                "obstacle_max_age_ms = 500\n"))
                .find("numeric IPv4"), std::string::npos);
}

TEST(BackendConfigTest, LoadsSelectedDeploymentProfileWhenProvided) {
  const char* path = std::getenv("EOS_TEST_BINDING_PATH");
  const char* profile = std::getenv("EOS_TEST_BINDING_PROFILE");
  if (path == nullptr || profile == nullptr) GTEST_SKIP() << "No deployment profile selected";
  const auto binding = eos::runtime::hardware_runtime::LoadBinding(
      path, profile, eos::runtime::hardware_runtime::manager::BuildBackendCatalog());
  EXPECT_FALSE(binding.devices.empty());
  for (const auto& device : binding.devices) {
    EXPECT_FALSE(device.part.kind.empty());
    EXPECT_FALSE(device.backend_type.empty());
  }
}

}  // namespace

TEST(BackendConfigTest, SimulatorVariantsKeepSeparateCapabilities) {
  const auto catalog = eos::runtime::hardware_runtime::manager::BuildBackendCatalog();
  const auto* demo = catalog.Find("demo");
  const auto* mujoco = catalog.Find("mujoco");
  ASSERT_NE(demo, nullptr);
  ASSERT_NE(mujoco, nullptr);
  EXPECT_TRUE(demo->compiled);
  EXPECT_TRUE(mujoco->compiled);
  EXPECT_TRUE(demo->supports_class("gimbal"));
  EXPECT_FALSE(mujoco->supports_class("gimbal"));
  EXPECT_TRUE(mujoco->supports_class("arm"));
  EXPECT_FALSE(mujoco->supports_class("mic"));
}

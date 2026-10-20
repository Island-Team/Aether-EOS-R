/// Drives the V4L2 camera backend through a scripted frame stream, so every
/// case runs without a camera: device presence, absence, a stream that ends,
/// halt, and the command paths a feed-only device must reject. The final cases
/// run the real opener, covering the device path a camera would gate.
#include "v4l2_cam.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace eos::backends::v4l2 {
namespace {

/// What a scripted open observed, so a test can assert the configuration
/// reached the device.
struct OpenRecord {
  std::mutex mutex;
  int calls{0};
  std::string video_device;
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  std::uint32_t fps{0};
};

struct Script {
  /// Frames served before the stream ends; `kEndless` keeps serving.
  static constexpr int kEndless = -1;
  int frames{kEndless};
  bool open_succeeds{true};
  /// What the opener reports as negotiated. Zero means the opener says nothing,
  /// which must leave the configured size standing.
  std::uint32_t negotiated_width_px{0};
  std::uint32_t negotiated_height_px{0};
  std::uint32_t negotiated_fps{0};
  std::atomic<bool> stopped{false};
  std::atomic<int> reads{0};
  std::shared_ptr<OpenRecord> record{std::make_shared<OpenRecord>()};
};

/// Declares a frame stream whose Nth frame is a small payload carrying N, so
/// the published frame is checkable byte by byte.
FrameOpen ScriptedOpen(const std::shared_ptr<Script>& script) {
  return [script](const V4l2CamConfig& config, FrameStream* stream) {
    {
      std::lock_guard<std::mutex> lock(script->record->mutex);
      ++script->record->calls;
      script->record->video_device = config.video_device;
      script->record->width_px = config.width_px;
      script->record->height_px = config.height_px;
      script->record->fps = config.fps;
    }
    if (!script->open_succeeds) return false;
    stream->width_px = script->negotiated_width_px;
    stream->height_px = script->negotiated_height_px;
    stream->fps = script->negotiated_fps;
    stream->read = [script](std::vector<std::uint8_t>* jpeg) {
      const int served = script->reads.fetch_add(1);
      if (script->frames != Script::kEndless && served >= script->frames) return false;
      jpeg->assign({0xff, 0xd8, static_cast<std::uint8_t>(served)});
      return true;
    };
    stream->stop = [script] { script->stopped = true; };
    return true;
  };
}

/// Polls until the backend publishes a frame, or the deadline passes.
bool WaitForFrame(const V4l2Cam& camera, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (camera.ReadCameraTelemetry().has_value()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return camera.ReadCameraTelemetry().has_value();
}

V4l2CamConfig Config() {
  V4l2CamConfig config;
  config.device_id = "v4l2-cam-0";
  config.video_device = "/dev/video0";
  config.width_px = 640;
  config.height_px = 480;
  config.fps = 30;
  config.read_timeout_ms = 100;
  return config;
}

TEST(V4l2CamTest, PublishesTheCapturedFrameThroughNeutralTelemetry) {
  const auto script = std::make_shared<Script>();
  script->negotiated_width_px = 640;
  script->negotiated_height_px = 480;
  script->negotiated_fps = 30;
  V4l2Cam camera(Config(), ScriptedOpen(script));

  const hardware::BackendIdentity identity = camera.Identify();
  EXPECT_EQ(identity.name, "v4l2-cam");
  EXPECT_EQ(identity.kind, hardware::DeviceKind::kCamera);
  EXPECT_EQ(identity.device_id, "v4l2-cam-0");
  const hardware::FeedDeclarations feeds = camera.DeclareFeeds();
  EXPECT_DOUBLE_EQ(feeds.sensor.max_rate_hz, 30.0);

  ASSERT_TRUE(WaitForFrame(camera, std::chrono::seconds(3)));
  const std::optional<hardware::CameraTelemetry> telemetry = camera.ReadCameraTelemetry();
  ASSERT_TRUE(telemetry.has_value());
  EXPECT_EQ(telemetry->width_px, 640u);
  EXPECT_EQ(telemetry->height_px, 480u);
  EXPECT_EQ(telemetry->frame_id, "camera");
  ASSERT_TRUE(telemetry->source_timestamp_ns.has_value());
  EXPECT_GT(*telemetry->source_timestamp_ns, 0);
  EXPECT_FALSE(telemetry->jpeg.empty());
  EXPECT_EQ(telemetry->jpeg[0], 0xff);
  EXPECT_EQ(telemetry->jpeg[1], 0xd8);
  EXPECT_TRUE(camera.ReadHealth().alive);

  // The configured device reached the capture side unaltered.
  std::lock_guard<std::mutex> lock(script->record->mutex);
  EXPECT_EQ(script->record->calls, 1);
  EXPECT_EQ(script->record->video_device, "/dev/video0");
  EXPECT_EQ(script->record->width_px, 640u);
  EXPECT_EQ(script->record->height_px, 480u);
  EXPECT_EQ(script->record->fps, 30u);
}

TEST(V4l2CamTest, RejectsADeviceThatNegotiatesAnotherMode) {
  const auto script = std::make_shared<Script>();
  script->negotiated_width_px = 1280;
  script->negotiated_height_px = 720;
  script->negotiated_fps = 25;
  V4l2Cam camera(Config(), ScriptedOpen(script));

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(camera.ReadCameraTelemetry().has_value());
  EXPECT_FALSE(camera.ReadHealth().alive);
}

TEST(V4l2CamTest, AnUnopenableDeviceIsAbsenceRatherThanABlankFrame) {
  const auto script = std::make_shared<Script>();
  script->negotiated_width_px = 640;
  script->negotiated_height_px = 480;
  script->negotiated_fps = 30;
  script->open_succeeds = false;
  V4l2Cam camera(Config(), ScriptedOpen(script));

  // Give the worker a chance to attempt the open.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(camera.ReadCameraTelemetry().has_value());
  const hardware::BackendHealth health = camera.ReadHealth();
  EXPECT_FALSE(health.alive);
  EXPECT_FALSE(health.admitted);
}

TEST(V4l2CamTest, AStreamThatEndsStopsReportingAFrame) {
  const auto script = std::make_shared<Script>();
  script->negotiated_width_px = 640;
  script->negotiated_height_px = 480;
  script->negotiated_fps = 30;
  script->frames = 1;
  V4l2Cam camera(Config(), ScriptedOpen(script));

  // The single frame is produced and then the stream ends. A dead device must
  // not keep serving its last frame, so the end state is absence, not a frozen
  // image. Wait on the teardown itself: health is also not-alive before the
  // first frame arrives, so it cannot mark this transition.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline && !script->stopped.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(script->stopped.load());
  EXPECT_FALSE(camera.ReadCameraTelemetry().has_value());
  EXPECT_FALSE(camera.ReadHealth().alive);
}

TEST(V4l2CamTest, ClampLeavesAContinuousCaptureAliveAndHaltStopsIt) {
  const auto script = std::make_shared<Script>();
  script->negotiated_width_px = 640;
  script->negotiated_height_px = 480;
  script->negotiated_fps = 30;
  V4l2Cam camera(Config(), ScriptedOpen(script));
  ASSERT_TRUE(WaitForFrame(camera, std::chrono::seconds(3)));

  // A camera has no actuation path and no last safe position, so a clamp is a
  // no-op: capture continues.
  camera.Clamp();
  EXPECT_TRUE(camera.ReadHealth().alive);
  EXPECT_FALSE(script->stopped.load());
  EXPECT_TRUE(WaitForFrame(camera, std::chrono::seconds(3)));

  camera.Halt();
  EXPECT_FALSE(camera.ReadCameraTelemetry().has_value());
  EXPECT_FALSE(camera.ReadHealth().alive);
}

TEST(V4l2CamTest, AFeedOnlyDeviceRejectsEveryCommandPath) {
  const auto script = std::make_shared<Script>();
  V4l2Cam camera(Config(), ScriptedOpen(script));

  hardware::MoveJCommand move_j{};
  hardware::MoveLCommand move_l{};
  hardware::BaseCommand base{};
  hardware::GimbalCommand gimbal{};
  hardware::GripperCommand gripper{};
  hardware::SpeakerCommand speaker{};
  hardware::BaseNavigateRelativeCommand navigation{};
  EXPECT_EQ(camera.ExecuteMoveJ(move_j), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteMoveL(move_l), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteBase(base), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteGimbal(gimbal), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteGripper(gripper), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteSpeaker(speaker), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecuteNavigateRelative(navigation), hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.ExecutePosture({hardware::BasePosture::kStand, "posture"}),
            hardware::ReleaseResult::kRejected);
  EXPECT_EQ(camera.StopMotion(), hardware::ReleaseResult::kRejected);
}

TEST(V4l2CamTest, RejectsAnIncompleteConfiguration) {
  V4l2CamConfig empty;
  EXPECT_THROW(
      { V4l2Cam camera(empty, ScriptedOpen(std::make_shared<Script>())); }, std::invalid_argument);

  V4l2CamConfig zero_width = Config();
  zero_width.width_px = 0;
  EXPECT_THROW(
      { V4l2Cam camera(zero_width, ScriptedOpen(std::make_shared<Script>())); },
      std::invalid_argument);

  V4l2CamConfig no_timeout = Config();
  no_timeout.read_timeout_ms = 0;
  EXPECT_THROW(
      { V4l2Cam camera(no_timeout, ScriptedOpen(std::make_shared<Script>())); },
      std::invalid_argument);

  V4l2CamConfig no_fps = Config();
  no_fps.fps = 0;
  EXPECT_THROW(
      { V4l2Cam camera(no_fps, ScriptedOpen(std::make_shared<Script>())); }, std::invalid_argument);

  V4l2CamConfig no_frame = Config();
  no_frame.frame_id.clear();
  EXPECT_THROW(
      { V4l2Cam camera(no_frame, ScriptedOpen(std::make_shared<Script>())); },
      std::invalid_argument);
}

TEST(V4l2CamTest, TheRealOpenPathReportsAbsenceForAMissingDevice) {
  // No injected opener here: this drives the real open and capability ioctls
  // against a path no driver answers. Absence is the honest result on a host
  // with no camera, and it must never publish a frame.
  V4l2CamConfig config = Config();
  config.video_device = "/dev/eos-no-such-camera";
  config.read_timeout_ms = 500;
  V4l2Cam camera(config);
  bool ever_had_a_frame = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    ever_had_a_frame = ever_had_a_frame || camera.ReadCameraTelemetry().has_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_FALSE(ever_had_a_frame);
  EXPECT_FALSE(camera.ReadHealth().alive);
}

TEST(V4l2CamTest, TheRealOpenPathReportsAbsenceForADeviceThatIsNotACamera) {
  // A file that opens but answers no V4L2 capability query is a missing camera,
  // not a broken one: the open succeeds and the capability check refuses it.
  V4l2CamConfig config = Config();
  config.video_device = "/dev/null";
  config.read_timeout_ms = 500;
  V4l2Cam camera(config);
  bool ever_had_a_frame = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    ever_had_a_frame = ever_had_a_frame || camera.ReadCameraTelemetry().has_value();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  EXPECT_FALSE(ever_had_a_frame);
  EXPECT_FALSE(camera.ReadHealth().alive);
}

}  // namespace
}  // namespace eos::backends::v4l2

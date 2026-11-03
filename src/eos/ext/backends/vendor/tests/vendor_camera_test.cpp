#include "vendor_camera.h"

#include <gtest/gtest.h>

#include <memory>

namespace {

class FakeCameraDriver final : public eos::backends::vendor::VendorCameraDriver {
 public:
  std::optional<eos::hardware::CameraTelemetry> Capture() override {
    ++captures;
    return eos::hardware::CameraTelemetry{
        .width_px = 2,
        .height_px = 1,
        .frame_id = "camera_link",
        .jpeg = {1, 2, 3},
        .depth_png = {4, 5, 6},
        .depth_scale = 0.00025,
        .depth_intrinsics = eos::hardware::CameraIntrinsics{
            .width_px = 2,
            .height_px = 1,
            .fx = 2.0,
            .fy = 2.0,
            .ppx = 1.0,
            .ppy = 0.5,
            .distortion_model = "inverse_brown_conrady"}};
  }
  void Stop() override { ++stops; }

  int captures{0};
  int stops{0};
};

TEST(VendorCameraTest, ServesDirectJpegTelemetryAndHalts) {
  auto driver = std::make_unique<FakeCameraDriver>();
  FakeCameraDriver* observed = driver.get();
  eos::backends::vendor::VendorCamera camera("cam-0", std::move(driver));

  EXPECT_EQ(camera.Identify().kind, eos::hardware::DeviceKind::kCamera);
  EXPECT_DOUBLE_EQ(camera.DeclareFeeds().sensor.max_rate_hz, 30.0);
  const auto frame = camera.ReadCameraTelemetry();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->width_px, 2U);
  EXPECT_EQ(frame->jpeg, std::vector<std::uint8_t>({1, 2, 3}));
  EXPECT_EQ(frame->depth_png, std::vector<std::uint8_t>({4, 5, 6}));
  EXPECT_DOUBLE_EQ(frame->depth_scale, 0.00025);
  ASSERT_TRUE(frame->depth_intrinsics.has_value());
  EXPECT_DOUBLE_EQ(frame->depth_intrinsics->fx, 2.0);
  EXPECT_EQ(observed->captures, 1);

  camera.Halt();
  EXPECT_EQ(observed->stops, 1);
  EXPECT_FALSE(camera.ReadHealth().alive);
  EXPECT_FALSE(camera.ReadCameraTelemetry().has_value());
}

TEST(VendorCameraTest, ClampIsExplicitNoOpForFeedOnlyDevice) {
  auto driver = std::make_unique<FakeCameraDriver>();
  FakeCameraDriver* observed = driver.get();
  eos::backends::vendor::VendorCamera camera("cam-0", std::move(driver));

  camera.Clamp();

  EXPECT_EQ(observed->stops, 0);
  EXPECT_TRUE(camera.ReadHealth().alive);
  EXPECT_TRUE(camera.ReadCameraTelemetry().has_value());
}

}  // namespace

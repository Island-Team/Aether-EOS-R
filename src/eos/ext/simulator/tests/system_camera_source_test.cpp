#include "src/eos/ext/simulator/system_camera_source.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace eos::ext::simulator {
namespace {

TEST(SystemCameraSourceTest, PublishesLatestValidFrameFromInjectedStream) {
  const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0xff, 0xc0, 0x00, 0x07, 0x08,
                                       0x00, 0x02, 0x00, 0x03, 0xff, 0xd9};
  auto reads = std::make_shared<unsigned>(0);
  SystemCameraSource source(
      {.width_px = 3, .height_px = 2, .fps = 30},
      [jpeg, reads](const SimulatorCameraConfig& config, CameraFrameStream* stream) {
        EXPECT_EQ(config.width_px, 3u);
        EXPECT_EQ(config.height_px, 2u);
        EXPECT_EQ(config.fps, 30u);
        stream->read = [jpeg, reads](std::vector<std::uint8_t>* output) {
          if ((*reads)++ == 0) {
            *output = jpeg;
            return CameraReadResult::kFrame;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          return CameraReadResult::kTimeout;
        };
        stream->stop = [] {};
        return true;
      });

  std::optional<CapturedCameraFrame> frame;
  for (int attempt = 0; attempt < 100 && !frame; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    frame = source.ReadFrame();
  }
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->frame.width_px, 3u);
  EXPECT_EQ(frame->frame.height_px, 2u);
  EXPECT_EQ(frame->frame.jpeg, jpeg);
  EXPECT_GT(frame->source_timestamp_ns, 0);
}

TEST(SystemCameraSourceTest, DropsInvalidJpegFrames) {
  auto reads = std::make_shared<unsigned>(0);
  SystemCameraSource source({.width_px = 3, .height_px = 2, .fps = 30},
                            [reads](const SimulatorCameraConfig&, CameraFrameStream* stream) {
                              stream->read = [reads](std::vector<std::uint8_t>* output) {
                                if ((*reads)++ == 0) {
                                  *output = {1, 2, 3};
                                  return CameraReadResult::kFrame;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                return CameraReadResult::kTimeout;
                              };
                              stream->stop = [] {};
                              return true;
                            });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_FALSE(source.ReadFrame().has_value());
}

TEST(SystemCameraSourceTest, RejectsAFrameOutsideTheConfiguredMode) {
  const std::vector<std::uint8_t> jpeg{0xff, 0xd8, 0xff, 0xc0, 0x00, 0x07, 0x08,
                                       0x00, 0x02, 0x00, 0x03, 0xff, 0xd9};
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  SystemCameraSource source(
      {.width_px = 1920, .height_px = 1080, .fps = 30},
      [jpeg, stopped](const SimulatorCameraConfig&, CameraFrameStream* stream) {
        stream->read = [jpeg](std::vector<std::uint8_t>* output) {
          *output = jpeg;
          return CameraReadResult::kFrame;
        };
        stream->stop = [stopped] { stopped->store(true); };
        return true;
      });
  for (int attempt = 0; attempt < 100 && !stopped->load(); ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(stopped->load());
  EXPECT_FALSE(source.ReadFrame().has_value());
}

}  // namespace
}  // namespace eos::ext::simulator

#include "src/eos/runtime/hardware/telemetry_hub.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/eos/lib/clock.h"
#include "src/eos/runtime/hardware/registry.h"

namespace eos::runtime::hardware_runtime {
namespace {

class CameraBackend final : public hardware::Backend {
 public:
  CameraBackend(std::string device_id, bool blocked)
      : device_id_(std::move(device_id)), blocked_(blocked) {}

  hardware::BackendIdentity Identify() const override {
    return {"test", "1.0.0", device_id_, hardware::DeviceKind::kCamera};
  }
  hardware::FeedDeclarations DeclareFeeds() const override { return {}; }
  hardware::BackendHealth ReadHealth() const override { return {true, true}; }
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    entered_cv_.notify_all();
    released_cv_.wait(lock, [this] { return !blocked_; });
    hardware::CameraTelemetry frame;
    frame.source_timestamp_ns = ++timestamp_;
    frame.jpeg = {1};
    return frame;
  }
  void Clamp() override {}
  void Halt() override {}

  void WaitUntilEntered() const {
    std::unique_lock<std::mutex> lock(mutex_);
    ASSERT_TRUE(entered_cv_.wait_for(lock, std::chrono::seconds(1), [this] { return entered_; }));
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    blocked_ = false;
    released_cv_.notify_all();
  }

 private:
  std::string device_id_;
  mutable std::mutex mutex_;
  mutable std::condition_variable entered_cv_;
  mutable std::condition_variable released_cv_;
  mutable bool entered_{false};
  bool blocked_{false};
  mutable std::int64_t timestamp_{0};
};

Device MakeCamera(std::unique_ptr<hardware::Backend> backend) {
  return Device(std::move(backend), "robot-0", PartConfig{}, hardware::FeedDeclarations{});
}

TEST(CameraHistoryTest, RetainsBoundedJpegsAndOnlyOneFullDepthFrame) {
  TelemetryHub::CameraHistory history;
  for (std::int64_t stamp = 1; stamp <= 194; ++stamp) {
    hardware::CameraTelemetry frame;
    frame.source_timestamp_ns = stamp;
    frame.jpeg = {static_cast<std::uint8_t>(stamp)};
    frame.depth_u16 = {static_cast<std::uint16_t>(stamp)};
    history.Push(std::move(frame), eos::lib::MonotonicTime{}, eos::lib::WallTime{});
  }
  ASSERT_EQ(history.Size(), 192u);
  EXPECT_FALSE(history.FrameAt(1));
  EXPECT_FALSE(history.FrameAt(2));
  EXPECT_TRUE(history.FrameAt(3));
  ASSERT_TRUE(history.FrameAt(194));
  EXPECT_EQ(history.FrameAt(194)->telemetry.jpeg,
            (std::vector<std::uint8_t>{194}));
  EXPECT_TRUE(history.FrameAt(194)->telemetry.depth_u16.empty());
  ASSERT_TRUE(history.Latest());
  EXPECT_EQ(history.Latest()->generation, 194u);
  EXPECT_EQ(history.Latest()->telemetry.depth_u16,
            (std::vector<std::uint16_t>{194}));

  hardware::CameraTelemetry duplicate;
  duplicate.source_timestamp_ns = 194;
  duplicate.jpeg = {99};
  history.Push(std::move(duplicate), eos::lib::MonotonicTime{}, eos::lib::WallTime{});
  EXPECT_EQ(history.Size(), 192u);
  EXPECT_EQ(history.Latest()->generation, 194u);
  EXPECT_EQ(history.Latest()->telemetry.jpeg, (std::vector<std::uint8_t>{194}));
}

TEST(TelemetryHubTest, BlockingCameraDoesNotBlockAnotherCamera) {
  auto blocked_backend = std::make_unique<CameraBackend>("cam-0", true);
  CameraBackend* blocked = blocked_backend.get();
  std::vector<Device> devices;
  devices.push_back(MakeCamera(std::move(blocked_backend)));
  devices.push_back(MakeCamera(std::make_unique<CameraBackend>("cam-1", false)));
  eos::lib::SystemClock clock;

  TelemetryHub hub(devices, clock);
  blocked->WaitUntilEntered();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!hub.SnapshotCamera("cam-1") && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  EXPECT_TRUE(hub.SnapshotCamera("cam-1"));
  EXPECT_FALSE(hub.SnapshotCamera("cam-0"));
  blocked->Release();
}

}  // namespace
}  // namespace eos::runtime::hardware_runtime

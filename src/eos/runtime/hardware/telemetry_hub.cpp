#include "src/eos/runtime/hardware/telemetry_hub.h"

#include <algorithm>
#include <chrono>
#include <utility>

#include "src/eos/runtime/hardware/registry.h"

namespace eos::runtime::hardware_runtime {

void TelemetryHub::CameraHistory::Push(hardware::CameraTelemetry telemetry,
                                       eos::lib::MonotonicTime received_at,
                                       eos::lib::WallTime observed_at) {
  const bool duplicate =
      latest_ &&
      (telemetry.source_timestamp_ns && latest_->telemetry.source_timestamp_ns
           ? telemetry.source_timestamp_ns == latest_->telemetry.source_timestamp_ns
           : telemetry.jpeg == latest_->telemetry.jpeg);
  if (duplicate) return;
  constexpr std::size_t kRetainedCameraFrames = 192;
  constexpr std::size_t kMaximumHistoryBytes = 128U * 1024U * 1024U;
  const std::uint64_t generation = latest_ ? latest_->generation + 1 : 1;
  hardware::CameraTelemetry archived;
  archived.width_px = telemetry.width_px;
  archived.height_px = telemetry.height_px;
  archived.frame_id = telemetry.frame_id;
  archived.source_timestamp_ns = telemetry.source_timestamp_ns;
  archived.source_clock = telemetry.source_clock;
  archived.source_clock_id = telemetry.source_clock_id;
  archived.jpeg = telemetry.jpeg;
  history_bytes_ += archived.jpeg.size();
  values_.push_back(CameraSnapshot{generation, received_at, observed_at, std::move(archived)});
  latest_ = CameraSnapshot{generation, received_at, observed_at, std::move(telemetry)};
  while (values_.size() > kRetainedCameraFrames || history_bytes_ > kMaximumHistoryBytes) {
    history_bytes_ -= values_.front().telemetry.jpeg.size();
    values_.pop_front();
  }
}

std::optional<TelemetryHub::CameraSnapshot> TelemetryHub::CameraHistory::Latest() const {
  return latest_;
}

std::optional<TelemetryHub::CameraSnapshot> TelemetryHub::CameraHistory::FrameAt(
    std::int64_t source_timestamp_ns) const {
  const auto found = std::find_if(
      values_.rbegin(), values_.rend(), [source_timestamp_ns](const CameraSnapshot& value) {
        return value.telemetry.source_timestamp_ns == source_timestamp_ns;
      });
  return found == values_.rend() ? std::nullopt : std::optional<CameraSnapshot>(*found);
}

TelemetryHub::TelemetryHub(const Registry& registry)
    : owned_clock_(std::make_unique<eos::lib::SystemClock>()), clock_(*owned_clock_) {
  StartWorkers(registry.devices());
}

TelemetryHub::TelemetryHub(const Registry& registry, const eos::lib::Clock& clock)
    : TelemetryHub(registry.devices(), clock) {}

TelemetryHub::TelemetryHub(const std::vector<Device>& devices, const eos::lib::Clock& clock)
    : clock_(clock) {
  StartWorkers(devices);
}

TelemetryHub::~TelemetryHub() {
  stopping_.store(true);
  stop_cv_.notify_all();
  for (auto& thread : threads_) {
    if (thread.joinable()) thread.join();
  }
}

std::optional<hardware::CameraTelemetry> TelemetryHub::LatestCamera(
    const std::string& device_id) const {
  const auto snapshot = SnapshotCamera(device_id);
  return snapshot ? std::optional<hardware::CameraTelemetry>(snapshot->telemetry) : std::nullopt;
}

std::optional<TelemetryHub::CameraSnapshot> TelemetryHub::SnapshotCamera(
    const std::string& device_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cameras_.find(device_id);
  return found == cameras_.end() ? std::nullopt : found->second.Latest();
}

std::optional<TelemetryHub::CameraSnapshot> TelemetryHub::FrameAt(
    const std::string& device_id, std::int64_t source_timestamp_ns) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cameras_.find(device_id);
  if (found == cameras_.end()) return std::nullopt;
  return found->second.FrameAt(source_timestamp_ns);
}

std::optional<hardware::MicrophoneTelemetry> TelemetryHub::LatestMicrophone(
    const std::string& device_id) const {
  const auto snapshot = SnapshotMicrophone(device_id);
  return snapshot ? std::optional<hardware::MicrophoneTelemetry>(snapshot->telemetry)
                  : std::nullopt;
}

std::optional<TelemetryHub::MicrophoneSnapshot> TelemetryHub::SnapshotMicrophone(
    const std::string& device_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = microphones_.find(device_id);
  return found == microphones_.end() || found->second.empty()
             ? std::nullopt
             : std::optional<MicrophoneSnapshot>(found->second.back());
}

std::optional<hardware::MicrophoneTelemetry> TelemetryHub::ConsumeMicrophone(
    const std::string& device_id, const std::string& consumer_id, bool start_at_latest) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = microphones_.find(device_id);
  if (found == microphones_.end() || found->second.empty()) return std::nullopt;
  const std::string key = consumer_id + "\n" + device_id;
  auto [cursor_entry, inserted] = microphone_cursors_.try_emplace(key, 0);
  std::uint64_t& cursor = cursor_entry->second;
  if (inserted && start_at_latest) cursor = found->second.back().generation - 1;
  const auto next =
      std::find_if(found->second.begin(), found->second.end(),
                   [cursor](const MicrophoneSnapshot& value) { return value.generation > cursor; });
  if (next == found->second.end()) return std::nullopt;
  cursor = next->generation;
  return next->telemetry;
}

void TelemetryHub::StartWorkers(const std::vector<Device>& devices) {
  for (const Device& device : devices) {
    const hardware::BackendIdentity identity = device.identity();
    const Device* const device_ptr = &device;
    if (identity.kind == hardware::DeviceKind::kCamera) {
      threads_.emplace_back([this, device_ptr, identity] { RunCamera(*device_ptr, identity); });
    } else if (identity.kind == hardware::DeviceKind::kMicrophone) {
      threads_.emplace_back(
          [this, device_ptr, identity] { RunMicrophone(*device_ptr, identity); });
    }
  }
}

void TelemetryHub::RunCamera(const Device& device, hardware::BackendIdentity identity) {
  using namespace std::chrono_literals;
  while (!stopping_.load()) {
    if (device.ReadHealth().alive) {
      if (auto telemetry = device.feeds().ReadCameraTelemetry()) {
        std::lock_guard<std::mutex> lock(mutex_);
        cameras_[identity.device_id].Push(std::move(*telemetry), clock_.MonotonicNow(),
                                           clock_.WallNow());
      }
    }
    if (WaitForStop(33ms)) return;
  }
}

void TelemetryHub::RunMicrophone(const Device& device, hardware::BackendIdentity identity) {
  using namespace std::chrono_literals;
  while (!stopping_.load()) {
    if (device.ReadHealth().alive) {
      if (auto telemetry = device.feeds().ReadMicrophoneTelemetry()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& values = microphones_[identity.device_id];
        const std::uint64_t generation = values.empty() ? 1 : values.back().generation + 1;
        values.push_back(MicrophoneSnapshot{generation, clock_.MonotonicNow(), clock_.WallNow(),
                                            std::move(*telemetry)});
        while (values.size() > 256) values.pop_front();
      }
    }
    if (WaitForStop(5ms)) return;
  }
}

bool TelemetryHub::WaitForStop(std::chrono::milliseconds duration) {
  std::unique_lock<std::mutex> lock(stop_mutex_);
  return stop_cv_.wait_for(lock, duration, [this] { return stopping_.load(); });
}

}  // namespace eos::runtime::hardware_runtime

#pragma once

#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "eos/hardware/backend.h"
#include "src/eos/lib/clock.h"

namespace eos::runtime::hardware_runtime {

class Registry;
class Device;

/// Single south-bound reader for camera and microphone feeds. Consumers read
/// independent snapshots/cursors, so observing a microphone never steals a
/// destructive PCM window from VAD.
class TelemetryHub final {
 public:
  struct CameraSnapshot {
    std::uint64_t generation{0};
    eos::lib::MonotonicTime received_at;
    eos::lib::WallTime observed_at;
    hardware::CameraTelemetry telemetry;
  };
  /// Bounded JPEG-only history for exact timestamp correlation. Latest keeps
  /// full RGB-D; history does not multiply aligned-depth memory by frame rate.
  class CameraHistory {
   public:
    void Push(hardware::CameraTelemetry telemetry, eos::lib::MonotonicTime received_at,
              eos::lib::WallTime observed_at);
    std::optional<CameraSnapshot> Latest() const;
    std::optional<CameraSnapshot> FrameAt(std::int64_t source_timestamp_ns) const;
    std::size_t Size() const { return values_.size(); }

   private:
    std::optional<CameraSnapshot> latest_;
    std::deque<CameraSnapshot> values_;
    std::size_t history_bytes_{0};
  };
  struct MicrophoneSnapshot {
    std::uint64_t generation{0};
    eos::lib::MonotonicTime received_at;
    eos::lib::WallTime observed_at;
    hardware::MicrophoneTelemetry telemetry;
  };
  explicit TelemetryHub(const Registry& registry);
  TelemetryHub(const Registry& registry, const eos::lib::Clock& clock);
  TelemetryHub(const std::vector<Device>& devices, const eos::lib::Clock& clock);
  ~TelemetryHub();
  TelemetryHub(const TelemetryHub&) = delete;
  TelemetryHub& operator=(const TelemetryHub&) = delete;

  std::optional<hardware::CameraTelemetry> LatestCamera(const std::string& device_id) const;
  std::optional<CameraSnapshot> SnapshotCamera(const std::string& device_id) const;
  /// Returns one recently captured camera frame by its exact source timestamp.
  std::optional<CameraSnapshot> FrameAt(const std::string& device_id,
                                        std::int64_t source_timestamp_ns) const;
  std::optional<hardware::MicrophoneTelemetry> LatestMicrophone(const std::string& device_id) const;
  std::optional<MicrophoneSnapshot> SnapshotMicrophone(const std::string& device_id) const;
  std::optional<hardware::MicrophoneTelemetry> ConsumeMicrophone(const std::string& device_id,
                                                                  const std::string& consumer_id,
                                                                  bool start_at_latest = false);

 private:
  void StartWorkers(const std::vector<Device>& devices);
  void RunCamera(const Device& device, hardware::BackendIdentity identity);
  void RunMicrophone(const Device& device, hardware::BackendIdentity identity);
  bool WaitForStop(std::chrono::milliseconds duration);

  std::unique_ptr<eos::lib::SystemClock> owned_clock_;
  const eos::lib::Clock& clock_;
  mutable std::mutex mutex_;
  std::map<std::string, CameraHistory> cameras_;
  std::map<std::string, std::deque<MicrophoneSnapshot>> microphones_;
  std::map<std::string, std::uint64_t> microphone_cursors_;
  std::atomic_bool stopping_{false};
  std::mutex stop_mutex_;
  std::condition_variable stop_cv_;
  std::vector<std::thread> threads_;
};

}  // namespace eos::runtime::hardware_runtime

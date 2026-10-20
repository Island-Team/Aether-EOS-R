/// Periodically submits frames from one selected camera to an object model.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "eos/hardware/backend.h"
#include "src/eos/runtime/models/runtime.h"

namespace eos::runtime::models {

bool FreshCameraFrame(std::chrono::steady_clock::time_point received_at,
                      std::chrono::steady_clock::time_point now,
                      std::chrono::milliseconds maximum_age = std::chrono::seconds(2));

class CameraObjectStore final {
 public:
  using Updated = std::function<void(const std::string&)>;

  struct Snapshot {
    std::string model_id;
    std::string device_id;
    std::string part_id;
    std::uint64_t generation{0};
    std::uint64_t sequence{0};
    std::string frame_id;
    std::optional<std::int64_t> source_timestamp_ns;
    hardware::SourceClockKind source_clock{hardware::SourceClockKind::kUnknown};
    std::string source_clock_id;
    std::uint32_t image_width_px{0};
    std::uint32_t image_height_px{0};
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
    nlohmann::json response;
  };

  void Set(Snapshot snapshot);
  std::optional<Snapshot> Latest() const;
  std::optional<Snapshot> Latest(const std::string& device_id) const;
  void SetUpdatedCallback(Updated updated);

 private:
  mutable std::mutex mutex_;
  mutable std::mutex update_mutex_;
  std::optional<Snapshot> latest_;
  std::map<std::string, Snapshot> by_device_;
  std::uint64_t sequence_{0};
  Updated updated_;
};

class CameraObjectPump final {
 public:
  using Capture = std::function<std::optional<hardware::CameraTelemetry>()>;

  CameraObjectPump(Runtime& models, Capture capture, CameraObjectStore& store, std::string model_id,
                   std::string device_id, std::string part_id, std::chrono::milliseconds interval);
  ~CameraObjectPump();
  CameraObjectPump(const CameraObjectPump&) = delete;
  CameraObjectPump& operator=(const CameraObjectPump&) = delete;

 private:
  void Run();

  Runtime& models_;
  Capture capture_;
  CameraObjectStore& store_;
  std::string model_id_;
  std::string device_id_;
  std::string part_id_;
  std::chrono::milliseconds interval_;
  std::mutex mutex_;
  std::condition_variable wakeup_;
  std::uint64_t next_generation_{0};
  bool stopping_{false};
  std::thread worker_;
};

}  // namespace eos::runtime::models

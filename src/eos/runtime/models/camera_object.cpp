#include "src/eos/runtime/models/camera_object.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include "src/eos/lib/log.h"

namespace eos::runtime::models {
namespace {

std::int64_t MonotonicNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Base64Encode(const std::vector<std::uint8_t>& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t a = input[offset];
    const std::uint32_t b = offset + 1 < input.size() ? input[offset + 1] : 0;
    const std::uint32_t c = offset + 2 < input.size() ? input[offset + 2] : 0;
    const std::uint32_t value = (a << 16) | (b << 8) | c;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[value & 0x3f] : '=');
  }
  return output;
}

}  // namespace

bool FreshCameraFrame(std::chrono::steady_clock::time_point received_at,
                      std::chrono::steady_clock::time_point now,
                      std::chrono::milliseconds maximum_age) {
  return maximum_age > std::chrono::milliseconds::zero() && received_at <= now &&
         now - received_at <= maximum_age;
}

void CameraObjectStore::Set(Snapshot snapshot) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  const std::string device_id = snapshot.device_id;
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.sequence = ++sequence_;
    by_device_[snapshot.device_id] = snapshot;
    latest_ = std::move(snapshot);
    updated = updated_;
  }
  if (updated) updated(device_id);
}

std::optional<CameraObjectStore::Snapshot> CameraObjectStore::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

std::optional<CameraObjectStore::Snapshot> CameraObjectStore::Latest(
    const std::string& device_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = by_device_.find(device_id);
  if (found == by_device_.end()) return std::nullopt;
  return found->second;
}

void CameraObjectStore::SetUpdatedCallback(Updated updated) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  updated_ = std::move(updated);
}

CameraObjectPump::CameraObjectPump(Runtime& models, Capture capture, CameraObjectStore& store,
                                   std::string model_id, std::string device_id, std::string part_id,
                                   std::chrono::milliseconds interval)
    : models_(models),
      capture_(std::move(capture)),
      store_(store),
      model_id_(std::move(model_id)),
      device_id_(std::move(device_id)),
      part_id_(std::move(part_id)),
      interval_(interval) {
  if (!capture_) throw std::invalid_argument("camera object capture callback is required");
  if (model_id_.empty() || device_id_.empty() || part_id_.empty()) {
    throw std::invalid_argument("camera object identity is required");
  }
  if (interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("camera object interval must be positive");
  }
  worker_ = std::thread(&CameraObjectPump::Run, this);
}

CameraObjectPump::~CameraObjectPump() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wakeup_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void CameraObjectPump::Run() {
  constexpr auto kMinimumFailureBackoff = std::chrono::milliseconds(1000);
  constexpr auto kMaximumFailureBackoff = std::chrono::milliseconds(30000);
  auto logger = eos::lib::GetLogger("models.camera_object");
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_object_started",
                     {{"model_id", model_id_},
                      {"device_id", device_id_},
                      {"part_id", part_id_},
                      {"interval_ms", interval_.count()}});
  auto next_capture = std::chrono::steady_clock::now();
  std::uint64_t consecutive_failures = 0;
  std::optional<std::size_t> last_logged_object_count;
  std::optional<std::int64_t> last_source_timestamp_ns;
  std::vector<std::uint8_t> last_jpeg;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    bool failed = false;
    std::string failure_reason;
    lock.unlock();
    try {
      const std::optional<hardware::CameraTelemetry> frame = capture_();
      if (frame && frame->width_px > 0 && frame->height_px > 0 && !frame->jpeg.empty()) {
        const bool duplicate =
            (frame->source_timestamp_ns && last_source_timestamp_ns &&
             frame->source_timestamp_ns == last_source_timestamp_ns) ||
            (!frame->source_timestamp_ns && !last_source_timestamp_ns && frame->jpeg == last_jpeg);
        if (!duplicate) {
          last_source_timestamp_ns = frame->source_timestamp_ns;
          last_jpeg = frame->jpeg;
          const std::uint64_t generation = ++next_generation_;
          const auto call_started = std::chrono::steady_clock::now();
          CallResult result = models_.Call(model_id_,
                                           {{"input",
                                             {{"data_b64", Base64Encode(frame->jpeg)},
                                              {"filename", "camera.jpg"},
                                              {"media_type", "image/jpeg"}}}},
                                           [this] {
                                             std::lock_guard<std::mutex> guard(mutex_);
                                             return stopping_;
                                           });
          const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - call_started)
                                       .count();
          if (result.status == CallResult::Status::kCompleted) {
            if (consecutive_failures > 0) {
              eos::lib::LogEvent(logger, spdlog::level::debug, "camera_object_recovered",
                                 {{"model_id", model_id_},
                                  {"generation", generation},
                                  {"failed_attempts", consecutive_failures}});
            }
            consecutive_failures = 0;
            const std::size_t object_count = result.response.value("object_count", 0);
            if (!last_logged_object_count || *last_logged_object_count != object_count) {
              eos::lib::LogEvent(logger, spdlog::level::debug, "camera_object_summary",
                                 {{"model_id", model_id_},
                                  {"device_id", device_id_},
                                  {"generation", generation},
                                  {"object_count", object_count},
                                  {"duration_ms", duration_ms}});
              last_logged_object_count = object_count;
            }
            store_.Set(
                {.model_id = model_id_,
                 .device_id = device_id_,
                 .part_id = part_id_,
                 .generation = generation,
                  .frame_id = frame->frame_id,
                  .source_timestamp_ns = frame->source_timestamp_ns.value_or(MonotonicNowNs()),
                  .source_clock = frame->source_timestamp_ns
                                      ? frame->source_clock
                                      : hardware::SourceClockKind::kSteadyBoot,
                  .source_clock_id = frame->source_timestamp_ns
                                         ? frame->source_clock_id
                                         : "eos-camera-object",
                 .image_width_px = frame->width_px,
                 .image_height_px = frame->height_px,
                 .jpeg = std::make_shared<const std::vector<std::uint8_t>>(frame->jpeg),
                 .response = std::move(result.response)});
          } else {
            failed = true;
            failure_reason = std::move(result.message);
          }
        }
      }
    } catch (const std::exception& error) {
      failed = true;
      failure_reason = error.what();
    }
    lock.lock();
    auto delay = interval_;
    if (failed) {
      ++consecutive_failures;
      if ((consecutive_failures & (consecutive_failures - 1)) == 0) {
        eos::lib::LogEvent(logger, spdlog::level::warn, "camera_object_failed",
                           {{"model_id", model_id_},
                            {"device_id", device_id_},
                            {"failed_attempts", consecutive_failures},
                            {"reason", failure_reason}});
      }
      const unsigned int exponent =
          static_cast<unsigned int>(std::min<std::uint64_t>(consecutive_failures, 8));
      delay = std::min(kMaximumFailureBackoff,
                       std::max(kMinimumFailureBackoff, interval_ * (1U << exponent)));
    }
    next_capture += delay;
    if (next_capture < std::chrono::steady_clock::now()) {
      next_capture = std::chrono::steady_clock::now();
    }
    wakeup_.wait_until(lock, next_capture, [this] { return stopping_; });
  }
  lock.unlock();
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_object_stopped",
                     {{"model_id", model_id_}, {"device_id", device_id_}});
}

}  // namespace eos::runtime::models

/// Continuous capture from the operating system's default camera.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "source_media.h"
#include "src/eos/ext/simulator/config.h"

namespace eos::ext::simulator {

struct CapturedCameraFrame {
  EncodedFrame frame;
  std::int64_t source_timestamp_ns{0};
};

class CameraFrameSource {
 public:
  virtual ~CameraFrameSource() = default;
  virtual std::optional<CapturedCameraFrame> ReadFrame() const = 0;
};

enum class CameraReadResult { kFrame, kTimeout, kEnd };

struct CameraFrameStream {
  std::function<CameraReadResult(std::vector<std::uint8_t>*)> read;
  std::function<void()> stop;
  std::function<std::string()> diagnostics;
};

using CameraStreamOpen = std::function<bool(const SimulatorCameraConfig&, CameraFrameStream*)>;

/// Keeps the newest JPEG from a restartable `autovideosrc` GStreamer pipeline.
/// The injectable opener is the device-free test seam.
class SystemCameraSource final : public CameraFrameSource {
 public:
  explicit SystemCameraSource(SimulatorCameraConfig config, CameraStreamOpen open = {});
  ~SystemCameraSource() override;

  SystemCameraSource(const SystemCameraSource&) = delete;
  SystemCameraSource& operator=(const SystemCameraSource&) = delete;

  std::optional<CapturedCameraFrame> ReadFrame() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::ext::simulator

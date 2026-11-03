/// Direct RealSense RGB-D camera adapter. It owns the device and exposes
/// bounded JPEG plus color-aligned depth telemetry. An independent latest-frame
/// WebSocket stream sends matched RGB-D data to the grasp service.
#pragma once

#include <memory>
#include <optional>
#include <string>

#include "eos/hardware/backend.h"

namespace eos::backends::vendor {

class VendorCameraDriver {
 public:
  virtual ~VendorCameraDriver() = default;
  virtual std::optional<hardware::CameraTelemetry> Capture() = 0;
  virtual void Stop() = 0;
};

class VendorCamera final : public hardware::Backend {
 public:
  explicit VendorCamera(std::string device_id);
  VendorCamera(std::string device_id, std::unique_ptr<VendorCameraDriver> driver);
  ~VendorCamera() override;

  hardware::BackendIdentity Identify() const override;
  hardware::FeedDeclarations DeclareFeeds() const override;
  hardware::BackendHealth ReadHealth() const override;
  std::optional<hardware::CameraTelemetry> ReadCameraTelemetry() const override;
  void Clamp() override;
  void Halt() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace eos::backends::vendor

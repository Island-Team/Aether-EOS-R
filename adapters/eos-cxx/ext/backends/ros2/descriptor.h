#pragma once
#include "adapters/eos-cxx/runtime/hardware/catalog.h"
#include "adapters/eos-cxx/ext/backends/ros2/config.h"
namespace eos::backends {
struct Ros2Settings final : runtime::hardware_runtime::BackendSettings {
  config::Ros2DeviceConfig value;
  std::uint64_t CameraFrameLimitBytes() const override { return value.max_image_bytes; }
};
runtime::hardware_runtime::BackendDescriptor Ros2Descriptor();
}

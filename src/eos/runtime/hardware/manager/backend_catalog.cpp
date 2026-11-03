#include "backend_catalog.h"

#include "src/eos/runtime/hardware/backends/demo/descriptor.h"
#include "src/eos/ext/backends/mujoco/descriptor.h"
#include "src/eos/ext/backends/ros2/descriptor.h"
#include "src/eos/ext/backends/vendor/audio/alsa/descriptor.h"
#include "src/eos/ext/backends/vendor/audio/aplay/descriptor.h"
#include "src/eos/ext/backends/vendor/camera/v4l2/descriptor.h"
#include "src/eos/ext/backends/vendor/descriptor.h"

namespace eos::runtime::hardware_runtime::manager {
BackendCatalog BuildBackendCatalog(
    const std::filesystem::path& simulator_socket) {
  BackendCatalog catalog;
  catalog.Register(backends::DemoDescriptor(simulator_socket));
  catalog.Register(backends::MujocoDescriptor(simulator_socket));
  catalog.Register(backends::Ros2Descriptor());
  catalog.Register(backends::AplayDescriptor());
  catalog.Register(backends::AlsaDescriptor());
  catalog.Register(backends::V4l2Descriptor());
  catalog.Register(backends::VendorDescriptor());
  return catalog;
}
}  // namespace eos::runtime::hardware_runtime::manager

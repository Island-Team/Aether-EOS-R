#include "builtin.h"

#include "demo/descriptor.h"
#include "ros2/descriptor.h"
#include "alsa/descriptor.h"
#include "aplay/descriptor.h"
#include "v4l2/descriptor.h"
namespace eos::backends {
runtime::hardware_runtime::BackendCatalog BuildBuiltinBackendCatalog(
    const std::filesystem::path& demo_socket) {
  runtime::hardware_runtime::BackendCatalog catalog;
  catalog.Register(DemoDescriptor(demo_socket));
  catalog.Register(Ros2Descriptor());
  catalog.Register(AplayDescriptor());
  catalog.Register(AlsaDescriptor());
  catalog.Register(V4l2Descriptor());
  return catalog;
}
}  // namespace eos::backends

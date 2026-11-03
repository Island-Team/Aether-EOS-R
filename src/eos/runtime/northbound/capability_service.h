/// Agent-facing capability discovery and operation lifecycle.
#pragma once

#include <grpcpp/grpcpp.h>

#include "eos/capabilities.grpc.pb.h"

namespace eos::runtime::dispatch {
class Dispatcher;
}
namespace eos::runtime::kernel {
class Broker;
}
namespace eos::runtime::northbound {

class CapabilityGrpcService final : public island::eos::v2::CapabilityService::Service {
 public:
  CapabilityGrpcService(kernel::Broker& broker, dispatch::Dispatcher& dispatcher)
      : broker_(broker), dispatcher_(dispatcher) {}

  grpc::Status ListCapabilities(grpc::ServerContext*,
                                const island::eos::v2::ListCapabilitiesRequest*,
                                island::eos::v2::ListCapabilitiesResponse*) override;
  grpc::Status SubmitOperation(grpc::ServerContext*, const island::eos::v2::SubmitOperationRequest*,
                               island::eos::v2::Operation*) override;
  grpc::Status GetOperation(grpc::ServerContext*, const island::eos::v2::OperationRequest*,
                            island::eos::v2::Operation*) override;
  grpc::Status WatchOperation(grpc::ServerContext*, const island::eos::v2::OperationRequest*,
                              grpc::ServerWriter<island::eos::v2::Operation>*) override;
  grpc::Status CancelOperation(grpc::ServerContext*, const island::eos::v2::OperationRequest*,
                               island::eos::v2::Operation*) override;

 private:
  kernel::Broker& broker_;
  dispatch::Dispatcher& dispatcher_;
};

}  // namespace eos::runtime::northbound

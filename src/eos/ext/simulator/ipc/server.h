#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_set>

#include <grpcpp/grpcpp.h>

#include "simulator.grpc.pb.h"

namespace eos::ext::simulator {
class World;
}

namespace eos::ext::simulator::ipc {

/// Private Unix-domain gRPC endpoint owned by the standalone simulator.
class SimulatorService final : public ::eos::simulator::v1::SimulatorService::Service {
 public:
  explicit SimulatorService(World& world) : world_(world) {}

  grpc::Status Health(grpc::ServerContext*, const ::eos::simulator::v1::HealthRequest*,
                      ::eos::simulator::v1::HealthResponse*) override;
  grpc::Status Execute(grpc::ServerContext*, const ::eos::simulator::v1::ExecuteRequest*,
                       ::eos::simulator::v1::ExecuteResponse*) override;
  grpc::Status ReadTelemetry(grpc::ServerContext*,
                             const ::eos::simulator::v1::ReadTelemetryRequest*,
                             ::eos::simulator::v1::ReadTelemetryResponse*) override;
  grpc::Status ReadCameraFrame(grpc::ServerContext*,
                               const ::eos::simulator::v1::ReadCameraFrameRequest*,
                               ::eos::simulator::v1::ReadCameraFrameResponse*) override;

  void Serve(const std::filesystem::path& socket_path, const std::atomic_bool& stop);

 private:
  bool DeviceAvailable(const std::string& id) const;
  World& world_;
  mutable std::mutex halted_mutex_;
  std::unordered_set<std::string> halted_devices_;
};

}  // namespace eos::ext::simulator::ipc

/// Internal gRPC server interface.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "src/eos/runtime/northbound/service.h"

namespace eos::runtime::skills {
class SkillManager;
}
namespace eos::runtime::actionlist {
class Manager;
}

namespace eos::internal {

struct NorthboundUdsOptions {
  std::filesystem::path path;
  std::uint32_t mode{0600};
};

class NorthboundServer {
 public:
  /// Defers listener binding until Run().
  NorthboundServer(std::string host, std::uint16_t grpc_port,
                   const eos::runtime::northbound::NorthboundServices& services,
                    std::function<void()> tick = nullptr,
                    eos::runtime::skills::SkillManager* skill_service = nullptr,
                    eos::runtime::actionlist::Manager* action_lists = nullptr,
                    std::filesystem::path workflow_templates = {},
                    NorthboundUdsOptions uds = {});
  ~NorthboundServer();

  NorthboundServer(const NorthboundServer&) = delete;
  NorthboundServer& operator=(const NorthboundServer&) = delete;

  /// Blocks on the owning thread until stopped.
  void Run();

  /// Call from the supervisor, not an async signal handler.
  void Stop();

  /// Port assigned after binding, including when configured as zero.
  std::uint16_t grpc_port() const;

  /// True after the listener binds.
  bool ready() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace eos::internal

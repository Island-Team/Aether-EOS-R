#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/kernel/catalog.h"
#include "src/eos/runtime/skills/worker/grants.h"

namespace eos::runtime::kernel {

// App-level syscall boundary for hardware capabilities. A caller gets a
// scoped token from the trusted execution owner, then names only a capability
// and command. Dispatcher retains validation, idempotency and safety release.
class Broker {
 public:
  Broker(const Catalog& catalog, dispatch::Dispatcher& dispatcher)
      : catalog_(catalog), dispatcher_(dispatcher) {}

  const Catalog& catalog() const { return catalog_; }

  std::string Grant(const std::string& capability, const std::string& execution_id,
                    std::uint64_t generation, std::chrono::steady_clock::time_point expires_at);
  dispatch::DispatchResult Execute(const std::string& capability, const std::string& token,
                                   const std::string& execution_id, std::uint64_t generation,
                                   nlohmann::json arguments, const std::string& command_id);
  dispatch::DispatchResult Start(const std::string& capability, const std::string& token,
                                 const std::string& execution_id, std::uint64_t generation,
                                 nlohmann::json arguments, const std::string& command_id);
  dispatch::DispatchResult Get(const std::string& command_id) const {
    return dispatcher_.GetCommand(command_id);
  }
  dispatch::DispatchResult Cancel(const std::string& command_id) {
    return dispatcher_.CancelCommand(command_id);
  }
  dispatch::DispatchResult StartTrustedCapability(const std::string& capability,
                                                  nlohmann::json arguments,
                                                  const std::string& command_id);
  dispatch::DispatchResult ExecuteTrustedCapability(
      const std::string& capability, nlohmann::json arguments,
      const std::string& command_id, const std::function<bool()>& cancelled = {});
  // For callers already inside the trusted runtime only. The process boundary
  // chooses this method; worker requests never carry a "trusted" flag.
  dispatch::DispatchResult ExecuteTrustedHardware(const std::string& primitive,
                                                  const std::string& version,
                                                  const std::string& device_id,
                                                  nlohmann::json arguments,
                                                  const std::string& command_id,
                                                  const std::function<bool()>& cancelled = {});
  dispatch::DispatchResult StartTrustedHardware(const std::string& primitive,
                                                const std::string& version,
                                                const std::string& device_id,
                                                nlohmann::json arguments,
                                                const std::string& command_id);
  void RevokeExecution(const std::string& execution_id) { grants_.RevokeExecution(execution_id); }
  void RevokeGeneration(std::uint64_t generation) { grants_.RevokeGeneration(generation); }

 private:
  const Catalog& catalog_;
  dispatch::Dispatcher& dispatcher_;
  skills::worker::GrantTable grants_;
};

}  // namespace eos::runtime::kernel

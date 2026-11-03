#include "broker.h"

#include <stdexcept>
#include <utility>

namespace eos::runtime::kernel {

std::string Broker::Grant(const std::string& capability, const std::string& execution_id,
                          std::uint64_t generation,
                          std::chrono::steady_clock::time_point expires_at) {
  const auto* resolved = catalog_.Find(capability);
  if (resolved == nullptr || !catalog_.Available(capability))
    throw std::invalid_argument("hardware capability is absent or unavailable: " + capability);
  if (resolved->primitive == "safety.estop" || resolved->primitive == "robot.stop" ||
      resolved->primitive == "motion.stop")
    throw std::invalid_argument("privileged safety capability cannot be granted to a worker");
  skills::worker::Grant grant;
  grant.execution_id = execution_id;
  grant.generation = generation;
  grant.primitive = resolved->primitive;
  grant.version = resolved->version;
  grant.device_id = resolved->device_id;
  grant.capability = capability;
  grant.expires_at = expires_at;
  return grants_.Issue(std::move(grant));
}

dispatch::DispatchResult Broker::Execute(const std::string& capability, const std::string& token,
                                          const std::string& execution_id,
                                          std::uint64_t generation, nlohmann::json arguments,
                                          const std::string& command_id) {
  const auto* resolved = catalog_.Find(capability);
  if (resolved == nullptr)
    return {dispatch::DispatchResult::Status::kNotFound, nullptr, "capability is not found"};
  const skills::worker::CapabilityCall call{token, execution_id, generation, resolved->primitive,
                                            resolved->version, resolved->device_id, capability};
  if (!grants_.Authorize(call))
    return {dispatch::DispatchResult::Status::kFailedPrecondition, nullptr,
            "capability grant is missing, expired or revoked"};
  return dispatcher_.ExecuteCommand(resolved->primitive, resolved->version, resolved->device_id,
                                    std::move(arguments), command_id);
}

dispatch::DispatchResult Broker::Start(const std::string& capability, const std::string& token,
                                        const std::string& execution_id,
                                        std::uint64_t generation, nlohmann::json arguments,
                                        const std::string& command_id) {
  const auto* resolved = catalog_.Find(capability);
  if (resolved == nullptr)
    return {dispatch::DispatchResult::Status::kNotFound, nullptr, "capability is not found"};
  const skills::worker::CapabilityCall call{token, execution_id, generation, resolved->primitive,
                                            resolved->version, resolved->device_id, capability};
  if (!grants_.Authorize(call))
    return {dispatch::DispatchResult::Status::kFailedPrecondition, nullptr,
            "capability grant is missing, expired or revoked"};
  return dispatcher_.StartCommand(resolved->primitive, resolved->version, resolved->device_id,
                                  std::move(arguments), command_id);
}

dispatch::DispatchResult Broker::StartTrustedCapability(const std::string& capability,
                                                         nlohmann::json arguments,
                                                         const std::string& command_id) {
  const auto* resolved = catalog_.Find(capability);
  if (resolved == nullptr)
    return {dispatch::DispatchResult::Status::kNotFound, nullptr, "capability is not found"};
  // Dispatcher owns replay, safety, and backend admission for a known target.
  return dispatcher_.StartCommand(resolved->primitive, resolved->version, resolved->device_id,
                                  std::move(arguments), command_id);
}

dispatch::DispatchResult Broker::ExecuteTrustedCapability(
    const std::string& capability, nlohmann::json arguments,
    const std::string& command_id, const std::function<bool()>& cancelled) {
  const auto* resolved = catalog_.Find(capability);
  if (resolved == nullptr)
    return {dispatch::DispatchResult::Status::kNotFound, nullptr, "capability is not found"};
  return dispatcher_.ExecuteCommand(resolved->primitive, resolved->version, resolved->device_id,
                                    std::move(arguments), command_id, cancelled);
}

dispatch::DispatchResult Broker::ExecuteTrustedHardware(const std::string& primitive,
                                                         const std::string& version,
                                                         const std::string& device_id,
                                                         nlohmann::json arguments,
                                                         const std::string& command_id,
                                                         const std::function<bool()>& cancelled) {
  if (primitive.empty() || version.empty())
    return {dispatch::DispatchResult::Status::kInvalidArgument, nullptr,
            "primitive and version are required"};
  // Legacy trusted callers may send a malformed or implicit target. Route
  // those requests through Dispatcher so it retains the contract-shaped
  // rejection and command replay semantics during the v1 migration.
  return dispatcher_.ExecuteCommand(primitive, version, device_id, std::move(arguments),
                                    command_id, cancelled);
}

dispatch::DispatchResult Broker::StartTrustedHardware(const std::string& primitive,
                                                       const std::string& version,
                                                       const std::string& device_id,
                                                       nlohmann::json arguments,
                                                       const std::string& command_id) {
  if (primitive.empty() || version.empty())
    return {dispatch::DispatchResult::Status::kInvalidArgument, nullptr,
            "primitive and version are required"};
  return dispatcher_.StartCommand(primitive, version, device_id, std::move(arguments), command_id);
}

}  // namespace eos::runtime::kernel

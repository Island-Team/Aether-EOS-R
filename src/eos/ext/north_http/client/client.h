/// Shared EOS gRPC client used by north HTTP process components.
#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>

#include "eos/gateway.grpc.pb.h"
#include "eos/skills.grpc.pb.h"
#include "eos/workflow.grpc.pb.h"

namespace eos::ext::north_http {

/// Owns the process-wide EOS channel and the service stubs created from it.
///
/// The object must outlive every HTTP or audit component that receives it.
class Client final {
 public:
  /// Creates one channel to `upstream` and one stub for each EOS northbound service.
  explicit Client(std::string upstream);

  /// Destroys the owned stubs and releases the shared channel reference.
  ~Client();

  /// Clients are process-owned and cannot be copied.
  Client(const Client&) = delete;

  /// Clients are process-owned and cannot be copy-assigned.
  Client& operator=(const Client&) = delete;

  /// Returns the single shared channel used by all service stubs.
  const std::shared_ptr<grpc::Channel>& channel() const noexcept;

  /// Returns the Gateway service stub bound to the shared channel.
  island::eos::v1::GatewayService::Stub& gateway() noexcept;

  /// Returns the Skill service stub bound to the shared channel.
  island::eos::v1::SkillService::Stub& skills() noexcept;

  /// Returns the Workflow service stub bound to the shared channel.
  island::eos::v1::WorkflowService::Stub& workflows() noexcept;

 private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<island::eos::v1::GatewayService::Stub> gateway_;
  std::unique_ptr<island::eos::v1::SkillService::Stub> skills_;
  std::unique_ptr<island::eos::v1::WorkflowService::Stub> workflows_;
};

}  // namespace eos::ext::north_http

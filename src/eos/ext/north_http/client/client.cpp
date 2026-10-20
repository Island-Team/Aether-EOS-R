#include "src/eos/ext/north_http/client/client.h"

#include <utility>

namespace eos::ext::north_http {

Client::Client(std::string upstream)
    : channel_([&upstream] {
        grpc::ChannelArguments arguments;
        arguments.SetMaxReceiveMessageSize(64 * 1024 * 1024);
        return grpc::CreateCustomChannel(std::move(upstream), grpc::InsecureChannelCredentials(),
                                         arguments);
      }()),
      gateway_(island::eos::v1::GatewayService::NewStub(channel_)),
      skills_(island::eos::v1::SkillService::NewStub(channel_)),
      workflows_(island::eos::v1::WorkflowService::NewStub(channel_)) {}

Client::~Client() = default;

const std::shared_ptr<grpc::Channel>& Client::channel() const noexcept { return channel_; }

island::eos::v1::GatewayService::Stub& Client::gateway() noexcept { return *gateway_; }

island::eos::v1::SkillService::Stub& Client::skills() noexcept { return *skills_; }

island::eos::v1::WorkflowService::Stub& Client::workflows() noexcept { return *workflows_; }

}  // namespace eos::ext::north_http

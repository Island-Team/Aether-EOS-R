#include "src/eos/runtime/models/runtime.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "model_provider.grpc.pb.h"

namespace eos::runtime::models {
namespace {
namespace wire = island::eos::external::models::v1;

class Provider final : public wire::ModelProviderService::Service {
 public:
  wire::CallRequest received;
  std::chrono::milliseconds delay{0};
  std::string response = R"({"ok":true})";

  grpc::Status Health(grpc::ServerContext*, const wire::HealthRequest*,
                      wire::HealthResponse* reply) override {
    reply->set_json(R"({"interface_version":"1.0","status":"ready","service":{"name":"test","version":"1"},"model":{"id":"provider-id","version":"1"},"capabilities":["audio.vad"]})");
    return grpc::Status::OK;
  }
  grpc::Status Call(grpc::ServerContext*, const wire::CallRequest* request,
                    wire::CallResponse* reply) override {
    received = *request;
    if (delay.count()) std::this_thread::sleep_for(delay);
    reply->set_json(response);
    return grpc::Status::OK;
  }
};

class GrpcModelTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&provider);
    server = builder.BuildAndStart();
    ASSERT_NE(server, nullptr);
    endpoint = "127.0.0.1:" + std::to_string(port);
  }
  void TearDown() override { server->Shutdown(); }
  ModelConfig Config() const {
    ModelConfig model;
    model.id = "test";
    model.provider_model_id = "provider-id";
    model.capability = Capability::kVad;
    model.endpoint = endpoint;
    model.transport = Transport::kGrpc;
    model.request_contract = "request";
    model.response_contract = "response";
    return model;
  }
  Provider provider;
  std::unique_ptr<grpc::Server> server;
  std::string endpoint;
};

TEST_F(GrpcModelTest, CallsProviderAndValidatesCanonicalJson) {
  int checked = 0;
  Runtime runtime({Config()}, [&](const std::string&, const nlohmann::json&) { ++checked; });
  const auto result = runtime.Call("test", {{"input", "sample"}});
  ASSERT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  EXPECT_EQ(result.response.at("ok"), true);
  EXPECT_EQ(provider.received.model_id(), "provider-id");
  EXPECT_EQ(provider.received.capability(), CapabilityName(Capability::kVad));
  EXPECT_EQ(nlohmann::json::parse(provider.received.json()).at("input"), "sample");
  EXPECT_EQ(checked, 2);
  for (int attempt = 0; attempt < 100; ++attempt) {
    const auto health = runtime.Health("test");
    if (health && health->status == HealthStatus::kReady) {
      EXPECT_EQ(health->provider_model_id, "provider-id");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  FAIL() << "gRPC health probe did not become ready";
}

TEST_F(GrpcModelTest, AppliesDeadlineAndResponseLimit) {
  auto model = Config();
  model.timeout = std::chrono::milliseconds(20);
  provider.delay = std::chrono::milliseconds(80);
  Runtime timed({model}, [](const std::string&, const nlohmann::json&) {});
  EXPECT_EQ(timed.Call("test", nlohmann::json::object()).status,
            CallResult::Status::kDeadlineExceeded);

  provider.delay = std::chrono::milliseconds(0);
  model.max_response_bytes = 2;
  Runtime bounded({model}, [](const std::string&, const nlohmann::json&) {});
  EXPECT_EQ(bounded.Call("test", nlohmann::json::object()).status,
            CallResult::Status::kProtocolError);
}

TEST_F(GrpcModelTest, RejectsInvalidCanonicalPayloads) {
  Runtime runtime({Config()}, [](const std::string& contract, const nlohmann::json& value) {
    if (contract == "request" && !value.contains("input")) {
      throw std::runtime_error("missing input");
    }
    if (contract == "response" && !value.contains("ok")) {
      throw std::runtime_error("missing ok");
    }
  });
  EXPECT_EQ(runtime.Call("test", nlohmann::json::object()).status,
            CallResult::Status::kInvalidArgument);
  provider.response = R"({"unexpected":true})";
  EXPECT_EQ(runtime.Call("test", {{"input", "sample"}}).status,
            CallResult::Status::kProtocolError);
}

TEST(GrpcModelConfigTest, ParsesLoopbackAndUnixTargets) {
  for (const std::string endpoint : {"127.0.0.1:16003", "unix:/tmp/eos-model.sock"}) {
    const auto models = ParseConfig("[[perception.model]]\nid = 'test'\ncapability = 'audio.vad'\n"
                                    "transport = 'grpc'\nendpoint = '" + endpoint + "'\n", "grpc.toml");
    ASSERT_EQ(models.size(), 1u);
    EXPECT_EQ(models.front().transport, Transport::kGrpc);
  }
  EXPECT_THROW(ParseConfig("[[perception.model]]\nid = 'test'\ncapability = 'audio.vad'\n"
                           "transport = 'grpc'\nendpoint = 'models.example:16003'\n", "bad.toml"),
               std::runtime_error);
}

TEST(GrpcModelTransportTest, CallsUnixSocketProvider) {
  Provider provider;
  const std::string path = "/tmp/island-eos-model-test-" + std::to_string(getpid()) + ".sock";
  std::filesystem::remove(path);
  grpc::ServerBuilder builder;
  builder.AddListeningPort("unix:" + path, grpc::InsecureServerCredentials());
  builder.RegisterService(&provider);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ModelConfig model;
  model.id = "test";
  model.capability = Capability::kVad;
  model.endpoint = "unix:" + path;
  model.transport = Transport::kGrpc;
  model.request_contract = "request";
  model.response_contract = "response";
  Runtime runtime({model}, [](const std::string&, const nlohmann::json&) {});
  const auto result = runtime.Call("test", {{"input", "sample"}});
  EXPECT_EQ(result.status, CallResult::Status::kCompleted) << result.message;
  server->Shutdown();
  std::filesystem::remove(path);
}

}  // namespace
}  // namespace eos::runtime::models

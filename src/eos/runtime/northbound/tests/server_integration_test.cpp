#include "src/eos/ext/backends/builtin.h"
#include <google/protobuf/empty.pb.h>
#include <google/protobuf/util/json_util.h>
#include <grpcpp/generic/generic_stub.h>
#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <boost/asio.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "eos/internal/northbound/server.h"
#include "eos/gateway.grpc.pb.h"
#include "eos/skills.grpc.pb.h"
#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/simulator/monitor.h"
#include "src/eos/lib/clock.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/models/runtime.h"
#include "src/eos/runtime/perception/body/reader.h"
#include "src/eos/runtime/skills/manager/execution.h"
#include "src/eos/runtime/skills/native/runtime.h"
#include "tests/eos/in_process_simulator.h"

namespace eos::runtime::northbound {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

constexpr const char* kRuntimeId = "018f22d2-82ab-7def-9234-56789abcdef0";

nlohmann::json ProtoJson(const google::protobuf::Message& message) {
  std::string text;
  google::protobuf::util::JsonPrintOptions options;
  options.always_print_primitive_fields = true;
  const auto status = google::protobuf::util::MessageToJsonString(message, &text, options);
  EXPECT_TRUE(status.ok()) << status.ToString();
  return nlohmann::json::parse(text);
}

std::string BufferBytes(const grpc::ByteBuffer& buffer) {
  std::vector<grpc::Slice> slices;
  EXPECT_TRUE(buffer.Dump(&slices).ok());
  std::string bytes;
  for (const auto& slice : slices) {
    bytes.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
  }
  return bytes;
}

struct CapturedLog {
  std::string correlation_id;
  std::string message;
};

class CorrelatedLogSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::vector<CapturedLog> Records() const {
    std::lock_guard<std::mutex> lock(records_mutex_);
    return records_;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& entry) override {
    std::lock_guard<std::mutex> lock(records_mutex_);
    records_.push_back(
        {lib::CurrentCorrelationId(), std::string(entry.payload.data(), entry.payload.size())});
  }
  void flush_() override {}

 private:
  mutable std::mutex records_mutex_;
  std::vector<CapturedLog> records_;
};

class GatewayServerIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    log_sink_ = std::make_shared<CorrelatedLogSink>();
    northbound_logger_ = lib::GetLogger("northbound");
    northbound_logger_->sinks().push_back(log_sink_);
    registry_ = std::make_unique<hardware_runtime::Registry>(hardware_runtime::Registry::Adopt(
        eos::backends::BuildBuiltinBackendCatalog(simulator_.socket_path()),
        hardware_runtime::LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo",
                                      eos::backends::BuildBuiltinBackendCatalog()),
        kRuntimeId));
    contracts_ = std::make_unique<init::SchemaStore>(EOS_CONTRACTS_ROOT);
    projection_ = std::make_unique<perception::body::Reader>(*registry_, clock_);
    dispatcher_ = std::make_unique<dispatch::Dispatcher>(*registry_, *contracts_);
    skills_ = std::make_unique<skills::native::Runtime>(
        skills::native::Runtime::LoadDirectory(EOS_TEST_SKILL_DIR, *dispatcher_));
    skill_service_ = std::make_unique<skills::ExecutionService>(*skills_);
    actionlist::SkillRunner skill_runner;
    skill_runner.start = [this](const std::string& name, const std::string& version,
                                nlohmann::json arguments, const std::string& request_id) {
      return skill_service_->Start(name, version, std::move(arguments), request_id);
    };
    skill_runner.cancel = [this](const std::string& id) { return skill_service_->Terminate(id); };
    skill_runner.list_executions = [this] { return skill_service_->ListExecutions(); };
    action_lists_ = std::make_unique<actionlist::Manager>(std::move(skill_runner));
    models::ModelConfig model;
    model.id = "test-vad";
    model.capability = models::Capability::kVad;
    model.endpoint = "http://127.0.0.1:1/v1/audio/vad";
    model.request_contract = "perception/model/vad_request.v1.json";
    model.response_contract = "perception/model/vad_response.v1.json";
    models_ = std::make_unique<models::Runtime>(std::vector<models::ModelConfig>{model});
    NorthboundServices services{projection_.get(), dispatcher_.get(), registry_.get(),
                                models_.get()};
    server_ = std::make_unique<internal::NorthboundServer>(
        "127.0.0.1", 0, services, nullptr, skill_service_.get(), action_lists_.get(),
        std::filesystem::path(EOS_CONTRACTS_ROOT) / "workflows");
    server_thread_ = std::thread([this] {
      try {
        server_->Run();
      } catch (const std::exception& error) {
        server_error_ = error.what();
        server_failed_.store(true);
      }
    });
    for (int i = 0; i < 500 && !server_failed_.load() && !server_->ready(); ++i) {
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_FALSE(server_failed_.load()) << server_error_;
    ASSERT_NE(server_->grpc_port(), 0);
    channel_ = grpc::CreateChannel("127.0.0.1:" + std::to_string(server_->grpc_port()),
                                   grpc::InsecureChannelCredentials());
    ASSERT_TRUE(channel_->WaitForConnected(std::chrono::system_clock::now() + 2s));
    stub_ = island::eos::v1::GatewayService::NewStub(channel_);
    skill_stub_ = island::eos::v1::SkillService::NewStub(channel_);
    workflow_stub_ = island::eos::v1::WorkflowService::NewStub(channel_);
  }

  void TearDown() override {
    if (server_) server_->Stop();
    if (server_thread_.joinable()) server_thread_.join();
    if (northbound_logger_) {
      auto& sinks = northbound_logger_->sinks();
      sinks.erase(std::remove(sinks.begin(), sinks.end(), log_sink_), sinks.end());
    }
  }

  test::InProcessSimulator simulator_;
  lib::ManualClock clock_;
  std::unique_ptr<hardware_runtime::Registry> registry_;
  std::unique_ptr<init::SchemaStore> contracts_;
  std::unique_ptr<perception::body::Reader> projection_;
  std::unique_ptr<dispatch::Dispatcher> dispatcher_;
  std::unique_ptr<skills::native::Runtime> skills_;
  std::unique_ptr<skills::ExecutionService> skill_service_;
  std::unique_ptr<actionlist::Manager> action_lists_;
  std::unique_ptr<models::Runtime> models_;
  std::unique_ptr<internal::NorthboundServer> server_;
  std::thread server_thread_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<island::eos::v1::GatewayService::Stub> stub_;
  std::unique_ptr<island::eos::v1::SkillService::Stub> skill_stub_;
  std::unique_ptr<island::eos::v1::WorkflowService::Stub> workflow_stub_;
  std::atomic<bool> server_failed_{false};
  std::string server_error_;
  std::shared_ptr<CorrelatedLogSink> log_sink_;
  std::shared_ptr<spdlog::logger> northbound_logger_;
};

TEST(GatewayServerLifecycleTest, StopShutsDownGrpcListener) {
  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services);
  std::atomic<bool> failed{false};
  std::thread server_thread([&] {
    try {
      server.Run();
    } catch (const std::exception&) {
      failed.store(true);
    }
  });
  for (int i = 0; i < 500 && !failed.load() && !server.ready(); ++i) {
    std::this_thread::sleep_for(1ms);
  }
  ASSERT_FALSE(failed.load());
  ASSERT_TRUE(server.ready());

  server.Stop();
  server_thread.join();
  EXPECT_FALSE(server.ready());
  EXPECT_FALSE(failed.load());
}

TEST(GatewayServerLifecycleTest, DoesNotBindLegacyHttpPort) {
  asio::io_context context;
  tcp::acceptor legacy_port(context, {asio::ip::make_address("127.0.0.1"), 16005});
  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services);
  std::thread server_thread([&] { server.Run(); });
  for (int i = 0; i < 500 && !server.ready(); ++i) std::this_thread::sleep_for(1ms);
  ASSERT_TRUE(server.ready());
  EXPECT_TRUE(legacy_port.is_open());
  server.Stop();
  server_thread.join();
}

TEST(GatewayServerLifecycleTest, ServesUnixSocketWithConfiguredPermissionsAndCleansUp) {
  const auto directory =
      std::filesystem::temp_directory_path() / ("eos-northbound-uds-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
  const auto socket_path = directory / "gateway.sock";

  // Leave a real stale socket at the path; startup may remove sockets but no other file type.
  const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(stale, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(socket_path.string().size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
  ASSERT_EQ(::bind(stale, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::close(stale), 0);

  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services, nullptr, nullptr, nullptr, {},
                                    {socket_path, 0660});
  std::atomic<bool> failed{false};
  std::thread server_thread([&] {
    try {
      server.Run();
    } catch (const std::exception&) {
      failed.store(true);
    }
  });
  for (int i = 0; i < 500 && !failed.load() && !server.ready(); ++i)
    std::this_thread::sleep_for(1ms);
  ASSERT_FALSE(failed.load());
  ASSERT_TRUE(server.ready());

  struct stat status{};
  ASSERT_EQ(::lstat(socket_path.c_str(), &status), 0);
  EXPECT_TRUE(S_ISSOCK(status.st_mode));
  EXPECT_EQ(status.st_mode & 0777, 0660);
  auto channel =
      grpc::CreateChannel("unix:" + socket_path.string(), grpc::InsecureChannelCredentials());
  EXPECT_TRUE(channel->WaitForConnected(std::chrono::system_clock::now() + 2s));

  server.Stop();
  server_thread.join();
  EXPECT_FALSE(std::filesystem::exists(socket_path));
  std::filesystem::remove_all(directory);
}

TEST(GatewayServerLifecycleTest, RefusesToRemoveSocketWithActiveListener) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-northbound-uds-live-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
  const auto socket_path = directory / "gateway.sock";

  const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  ASSERT_LT(socket_path.string().size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.string().size() + 1);
  ASSERT_EQ(::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ASSERT_EQ(::listen(listener, 1), 0);

  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services, nullptr, nullptr, nullptr, {},
                                    {socket_path, 0600});
  EXPECT_THROW(server.Run(), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_socket(socket_path));

  const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_GE(client, 0);
  EXPECT_EQ(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
  ::close(client);
  ::close(listener);
  std::filesystem::remove_all(directory);
}

TEST(GatewayServerLifecycleTest, RefusesToRemoveNonSocketAtUnixPath) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-northbound-uds-file-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  ASSERT_EQ(::chmod(directory.c_str(), 0700), 0);
  const auto socket_path = directory / "gateway.sock";
  std::ofstream(socket_path) << "keep";

  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services, nullptr, nullptr, nullptr, {},
                                    {socket_path, 0600});
  EXPECT_THROW(server.Run(), std::runtime_error);
  EXPECT_TRUE(std::filesystem::is_regular_file(socket_path));
  std::filesystem::remove_all(directory);
}

TEST(GatewayServerLifecycleTest, RefusesUnixSocketBelowSymlinkedParent) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("eos-northbound-uds-link-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directory(root));
  ASSERT_EQ(::chmod(root.c_str(), 0700), 0);
  ASSERT_TRUE(std::filesystem::create_directory(root / "real"));
  std::filesystem::create_directory_symlink(root / "real", root / "link");
  ASSERT_TRUE(std::filesystem::is_symlink(root / "link"));
  const auto socket_path = root / "link" / "gateway.sock";

  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services, nullptr, nullptr, nullptr, {},
                                    {socket_path, 0600});
  EXPECT_THROW(server.Run(), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(root / "real" / "gateway.sock"));
  std::filesystem::remove_all(root);
}

TEST(GatewayServerLifecycleTest, RefusesUnixSocketInNonPrivateParent) {
  const auto directory = std::filesystem::temp_directory_path() /
                         ("eos-northbound-uds-public-" + std::to_string(::getpid()));
  std::filesystem::remove_all(directory);
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  ASSERT_EQ(::chmod(directory.c_str(), 0750), 0);

  NorthboundServices services;
  internal::NorthboundServer server("127.0.0.1", 0, services, nullptr, nullptr, nullptr, {},
                                    {directory / "gateway.sock", 0600});
  EXPECT_THROW(server.Run(), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory / "gateway.sock"));
  std::filesystem::remove_all(directory);
}

TEST_F(GatewayServerIntegrationTest, NativeGrpcGatewayServesUnaryMethods) {
  google::protobuf::Empty empty;
  grpc::ClientContext discover_context;
  island::eos::v1::DiscoverResponse discovery;
  ASSERT_TRUE(stub_->Discover(&discover_context, empty, &discovery).ok());
  EXPECT_EQ(discovery.robot().id(), kRuntimeId);
  const auto base = std::find_if(discovery.devices().begin(), discovery.devices().end(),
                                 [](const auto& device) { return device.id() == "base-0"; });
  ASSERT_NE(base, discovery.devices().end());
  EXPECT_EQ(base->state_uri(), "robots/demo-0/devices/base-0/state");
  const auto navigate =
      std::find_if(base->primitives().begin(), base->primitives().end(),
                   [](const auto& primitive) { return primitive.name() == "base.navigate"; });
  ASSERT_NE(navigate, base->primitives().end());
  EXPECT_EQ(navigate->parameters().fields().at("type").string_value(), "object");
  EXPECT_EQ(
      navigate->parameters().fields().at("properties").struct_value().fields().count("reference"),
      1u);

  grpc::ClientContext status_context;
  google::protobuf::Struct status;
  ASSERT_TRUE(stub_->GetStatus(&status_context, empty, &status).ok());
  EXPECT_EQ(status.fields().at("robot_id").string_value(), kRuntimeId);

  grpc::ClientContext resources_context;
  google::protobuf::ListValue resources;
  ASSERT_TRUE(stub_->ListResources(&resources_context, empty, &resources).ok());
  EXPECT_GT(resources.values_size(), 0);
  const auto has_base_status =
      std::find_if(resources.values().begin(), resources.values().end(), [](const auto& resource) {
        const auto& fields = resource.struct_value().fields();
        const auto uri = fields.find("uri");
        return uri != fields.end() && uri->second.string_value() == "robots/demo-0/base/status";
      });
  ASSERT_NE(has_base_status, resources.values().end());

  island::eos::v1::ResourceRequest status_request;
  status_request.set_uri("robots/demo-0/base/status");
  grpc::ClientContext read_status_context;
  google::protobuf::Struct base_status;
  ASSERT_TRUE(stub_->ReadResource(&read_status_context, status_request, &base_status).ok());
  EXPECT_EQ(base_status.fields().at("type").string_value(), "base.status");
  EXPECT_NE(read_status_context.GetServerInitialMetadata().find("eos-sample-age-ms"),
            read_status_context.GetServerInitialMetadata().end());

  island::eos::v1::CallPrimitiveRequest call;
  call.set_name("base.navigate");
  call.set_version("1.0.0");
  call.set_device_id("base-0");
  call.set_command_id("grpc-only-1");
  auto* arguments = call.mutable_arguments()->mutable_fields();
  (*arguments)["reference"].set_string_value("relative");
  (*arguments)["x_m"].set_number_value(0.0);
  (*arguments)["y_m"].set_number_value(0.0);
  (*arguments)["yaw_rad"].set_number_value(0.0);
  grpc::ClientContext call_context;
  google::protobuf::Struct acknowledgement;
  ASSERT_TRUE(stub_->CallPrimitive(&call_context, call, &acknowledgement).ok());
  EXPECT_EQ(acknowledgement.fields().at("accepted").bool_value(), true);
}

TEST_F(GatewayServerIntegrationTest, GeneratedStubExposesTypedPerceptionMethods) {
  island::eos::v1::InferVadRequest vad;
  vad.set_model_id("test-vad");
  vad.mutable_input()->set_data("RIFF");
  grpc::ClientContext vad_context;
  island::eos::v1::InferVadResponse vad_response;
  const grpc::Status vad_status = stub_->InferVad(&vad_context, vad, &vad_response);
  EXPECT_TRUE(vad_status.error_code() == grpc::StatusCode::UNAVAILABLE ||
              vad_status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED);

  island::eos::v1::DetectHumansRequest humans;
  humans.set_model_id("test-vad");
  humans.mutable_input()->set_data("image");
  grpc::ClientContext humans_context;
  island::eos::v1::DetectHumansResponse humans_response;
  EXPECT_EQ(stub_->DetectHumans(&humans_context, humans, &humans_response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  island::eos::v1::EstimateHumanPoseRequest pose;
  pose.set_model_id("test-vad");
  pose.mutable_input()->set_data("image");
  grpc::ClientContext pose_context;
  island::eos::v1::EstimateHumanPoseResponse pose_response;
  EXPECT_EQ(stub_->EstimateHumanPose(&pose_context, pose, &pose_response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  island::eos::v1::DetectFacesRequest faces;
  faces.set_model_id("test-vad");
  faces.mutable_input()->set_data("image");
  grpc::ClientContext faces_context;
  island::eos::v1::DetectFacesResponse faces_response;
  EXPECT_EQ(stub_->DetectFaces(&faces_context, faces, &faces_response).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(GatewayServerIntegrationTest, PropagatesAndValidatesRequestIdMetadata) {
  google::protobuf::Empty request;
  google::protobuf::Struct response;
  grpc::ClientContext correlated;
  correlated.AddMetadata(eos::lib::kRequestIdMetadataKey, "adapter-trace-42");
  ASSERT_TRUE(stub_->GetStatus(&correlated, request, &response).ok());
  EXPECT_EQ(response.fields().at("correlation_id").string_value(), "adapter-trace-42");

  grpc::ClientContext invalid;
  invalid.AddMetadata(eos::lib::kRequestIdMetadataKey, "invalid request id");
  const grpc::Status invalid_status = stub_->GetStatus(&invalid, request, &response);
  EXPECT_EQ(invalid_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  grpc::ClientContext duplicate;
  duplicate.AddMetadata(eos::lib::kRequestIdMetadataKey, "first");
  duplicate.AddMetadata(eos::lib::kRequestIdMetadataKey, "second");
  const grpc::Status duplicate_status = stub_->GetStatus(&duplicate, request, &response);
  EXPECT_EQ(duplicate_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GatewayServerIntegrationTest, DedicatedSkillServiceSupportsDiscoveryAndLifecycle) {
  grpc::ClientContext list_context;
  island::eos::v1::ListSkillsRequest list;
  island::eos::v1::ListSkillsResponse listed;
  ASSERT_TRUE(skill_stub_->ListSkills(&list_context, list, &listed).ok());
  EXPECT_EQ(listed.skills_size(), 12);
  island::eos::v1::GetSkillRequest get_skill;
  get_skill.mutable_reference()->set_name("fixture.sequence");
  get_skill.mutable_reference()->set_version("1.0.0");
  grpc::ClientContext get_skill_context;
  island::eos::v1::SkillDescriptor descriptor;
  ASSERT_TRUE(skill_stub_->GetSkill(&get_skill_context, get_skill, &descriptor).ok());
  EXPECT_EQ(descriptor.reference().name(), "fixture.sequence");

  island::eos::v1::StartExecutionRequest start;
  start.mutable_reference()->set_name("fixture.sequence");
  start.mutable_reference()->set_version("1.0.0");
  start.mutable_arguments();
  start.set_request_id("async-grpc-request");
  grpc::ClientContext start_context;
  island::eos::v1::SkillExecution started;
  const grpc::Status start_status = skill_stub_->StartExecution(&start_context, start, &started);
  ASSERT_TRUE(start_status.ok()) << start_status.error_message();
  EXPECT_FALSE(started.execution_id().empty());
  EXPECT_EQ(started.request_id(), "async-grpc-request");
  EXPECT_FALSE(started.has_deadline_at());

  grpc::ClientContext replay_context;
  island::eos::v1::SkillExecution replayed;
  ASSERT_TRUE(skill_stub_->StartExecution(&replay_context, start, &replayed).ok());
  EXPECT_EQ(replayed.execution_id(), started.execution_id());

  island::eos::v1::WatchExecutionRequest watch;
  watch.set_execution_id(started.execution_id());
  grpc::ClientContext watch_context;
  auto reader = skill_stub_->WatchExecution(&watch_context, watch);
  island::eos::v1::SkillExecution event;
  std::vector<island::eos::v1::ExecutionStatus> states;
  while (reader->Read(&event)) states.push_back(event.status());
  EXPECT_TRUE(reader->Finish().ok());
  ASSERT_GE(states.size(), 3u);
  EXPECT_EQ(states.front(), island::eos::v1::EXECUTION_STATUS_QUEUED);
  EXPECT_EQ(states.back(), island::eos::v1::EXECUTION_STATUS_SUCCEEDED);

  island::eos::v1::GetExecutionRequest get;
  get.set_execution_id(started.execution_id());
  grpc::ClientContext get_context;
  island::eos::v1::SkillExecution completed;
  ASSERT_TRUE(skill_stub_->GetExecution(&get_context, get, &completed).ok());
  EXPECT_EQ(completed.status(), island::eos::v1::EXECUTION_STATUS_SUCCEEDED);
  EXPECT_TRUE(completed.result().fields().contains("fixture"));
  EXPECT_GT(completed.latest_journal_sequence(), 0u);

  island::eos::v1::WatchExecutionRequest terminal_watch;
  terminal_watch.set_execution_id(started.execution_id());
  terminal_watch.set_after_sequence(completed.sequence());
  grpc::ClientContext terminal_watch_context;
  auto terminal_reader = skill_stub_->WatchExecution(&terminal_watch_context, terminal_watch);
  island::eos::v1::SkillExecution terminal_event;
  EXPECT_FALSE(terminal_reader->Read(&terminal_event));
  EXPECT_TRUE(terminal_reader->Finish().ok());

  island::eos::v1::StartExecutionRequest failing_start;
  failing_start.mutable_reference()->set_name("fixture.reported_failure");
  failing_start.mutable_reference()->set_version("1.0.0");
  failing_start.mutable_arguments();
  failing_start.set_request_id("diagnostic-grpc-request");
  failing_start.mutable_timeout()->set_seconds(10);
  grpc::ClientContext failing_start_context;
  island::eos::v1::SkillExecution failed;
  ASSERT_TRUE(skill_stub_->StartExecution(&failing_start_context, failing_start, &failed).ok());
  island::eos::v1::GetExecutionRequest get_failed;
  get_failed.set_execution_id(failed.execution_id());
  for (int attempt = 0; attempt < 100; ++attempt) {
    grpc::ClientContext failed_context;
    ASSERT_TRUE(skill_stub_->GetExecution(&failed_context, get_failed, &failed).ok());
    if (failed.status() == island::eos::v1::EXECUTION_STATUS_FAILED) break;
    std::this_thread::sleep_for(2ms);
  }
  EXPECT_EQ(failed.status(), island::eos::v1::EXECUTION_STATUS_FAILED);
  EXPECT_EQ(failed.reason_code(), "fixture.backend_failed");
  EXPECT_EQ(failed.message(), "fixture backend was unavailable");
  const nlohmann::json failed_json = ProtoJson(failed);
  EXPECT_EQ(failed_json.at("reasonCode"), "fixture.backend_failed");
  EXPECT_EQ(failed_json.at("message"), "fixture backend was unavailable");

  grpc::ClientContext list_executions_context;
  island::eos::v1::ListExecutionsRequest list_executions;
  island::eos::v1::ListExecutionsResponse executions;
  ASSERT_TRUE(
      skill_stub_->ListExecutions(&list_executions_context, list_executions, &executions).ok());
  EXPECT_NE(std::find_if(executions.executions().begin(), executions.executions().end(),
                         [&](const auto& execution) {
                           return execution.execution_id() == started.execution_id();
                         }),
            executions.executions().end());

  island::eos::v1::FollowExecutionJournalRequest watch_events;
  watch_events.set_execution_id(started.execution_id());
  grpc::ClientContext watch_events_context;
  watch_events_context.AddMetadata(lib::kRequestIdMetadataKey, "event-watch-correlation");
  auto event_reader = skill_stub_->FollowExecutionJournal(&watch_events_context, watch_events);
  island::eos::v1::SkillJournalEntry execution_event;
  std::uint64_t last_journal_sequence = 0;
  std::size_t event_count = 0;
  std::size_t record_count = 0;
  while (event_reader->Read(&execution_event)) {
    EXPECT_GT(execution_event.journal_sequence(), last_journal_sequence);
    EXPECT_TRUE(execution_event.has_snapshot() || execution_event.has_record());
    if (execution_event.has_record()) {
      EXPECT_FALSE(execution_event.record().name().empty());
      ++record_count;
    }
    last_journal_sequence = execution_event.journal_sequence();
    ++event_count;
  }
  EXPECT_TRUE(event_reader->Finish().ok());
  EXPECT_GE(event_count, 3u);
  EXPECT_GT(record_count, 0u);
  EXPECT_EQ(last_journal_sequence, completed.latest_journal_sequence());
  const auto records = log_sink_->Records();
  const auto closed = std::find_if(records.begin(), records.end(), [](const CapturedLog& record) {
    return record.message.find("[skill_event_watch_closed]") != std::string::npos;
  });
  ASSERT_NE(closed, records.end());
  EXPECT_EQ(closed->correlation_id, "event-watch-correlation");
  EXPECT_NE(closed->message.find("method=FollowExecutionJournal"), std::string::npos);
  EXPECT_NE(closed->message.find("status=0"), std::string::npos);
  EXPECT_NE(closed->message.find("terminal=true"), std::string::npos);
  EXPECT_NE(closed->message.find("events=" + std::to_string(event_count)), std::string::npos);
  EXPECT_NE(closed->message.find("duration_ms="), std::string::npos);

  island::eos::v1::FollowExecutionJournalRequest terminal_cursor;
  terminal_cursor.set_execution_id(started.execution_id());
  terminal_cursor.set_after_journal_sequence(last_journal_sequence);
  grpc::ClientContext terminal_cursor_context;
  terminal_cursor_context.set_deadline(std::chrono::system_clock::now() + 1s);
  terminal_cursor_context.AddMetadata(lib::kRequestIdMetadataKey, "event-watch-terminal-cursor");
  auto terminal_journal_reader =
      skill_stub_->FollowExecutionJournal(&terminal_cursor_context, terminal_cursor);
  island::eos::v1::SkillJournalEntry ignored;
  EXPECT_FALSE(terminal_journal_reader->Read(&ignored));
  EXPECT_TRUE(terminal_journal_reader->Finish().ok());
  std::vector<CapturedLog> terminal_records;
  auto terminal_closed = terminal_records.end();
  for (int attempt = 0; attempt < 50; ++attempt) {
    terminal_records = log_sink_->Records();
    terminal_closed = std::find_if(terminal_records.begin(), terminal_records.end(),
                                    [](const CapturedLog& record) {
                                      return record.correlation_id == "event-watch-terminal-cursor" &&
                                             record.message.find("[skill_event_watch_closed]") !=
                                                 std::string::npos;
                                    });
    if (terminal_closed != terminal_records.end()) break;
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_NE(terminal_closed, terminal_records.end());
  EXPECT_NE(terminal_closed->message.find("cancelled=false"), std::string::npos);
  EXPECT_NE(terminal_closed->message.find("terminal=true"), std::string::npos);

  island::eos::v1::SignalExecutionRequest signal;
  signal.set_execution_id(started.execution_id());
  signal.set_request_id("unsupported-v1-signal");
  signal.set_name("control.update");
  signal.mutable_payload();
  island::eos::v1::SkillSignalReceipt receipt;
  grpc::ClientContext signal_context;
  EXPECT_EQ(skill_stub_->SignalExecution(&signal_context, signal, &receipt).error_code(),
            grpc::StatusCode::FAILED_PRECONDITION);

  island::eos::v1::WatchExecutionRequest resumed;
  resumed.set_execution_id(started.execution_id());
  resumed.set_after_sequence(1);
  grpc::ClientContext resumed_context;
  auto resumed_reader = skill_stub_->WatchExecution(&resumed_context, resumed);
  island::eos::v1::SkillExecution resumed_snapshot;
  ASSERT_TRUE(resumed_reader->Read(&resumed_snapshot));
  EXPECT_GT(resumed_snapshot.sequence(), 1u);
  while (resumed_reader->Read(&resumed_snapshot)) {
  }
  EXPECT_TRUE(resumed_reader->Finish().ok());
}

TEST_F(GatewayServerIntegrationTest, SkillRequestsSupportOptionalTimeoutAndIdempotencyConflict) {
  island::eos::v1::StartExecutionRequest start;
  start.mutable_reference()->set_name("fixture.sequence");
  start.mutable_reference()->set_version("1.0.0");
  start.mutable_arguments();
  start.set_request_id("conflicting-request");
  grpc::ClientContext invalid_context;
  island::eos::v1::SkillExecution response;
  ASSERT_TRUE(skill_stub_->StartExecution(&invalid_context, start, &response).ok());
  EXPECT_FALSE(response.has_deadline_at());

  start.mutable_timeout();
  grpc::ClientContext first_context;
  EXPECT_EQ(skill_stub_->StartExecution(&first_context, start, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  start.mutable_timeout()->set_seconds(5);
  grpc::ClientContext conflict_context;
  EXPECT_EQ(skill_stub_->StartExecution(&conflict_context, start, &response).error_code(),
            grpc::StatusCode::ALREADY_EXISTS);

  island::eos::v1::StartExecutionRequest cancellable;
  cancellable.mutable_reference()->set_name("fixture.uncooperative");
  cancellable.mutable_reference()->set_version("1.0.0");
  cancellable.mutable_arguments();
  cancellable.set_request_id("cancel-request");
  cancellable.mutable_timeout()->set_seconds(5);
  grpc::ClientContext cancellable_context;
  ASSERT_TRUE(skill_stub_->StartExecution(&cancellable_context, cancellable, &response).ok());
  EXPECT_TRUE(response.has_deadline_at());
  island::eos::v1::TerminateExecutionRequest cancel;
  cancel.set_execution_id(response.execution_id());
  grpc::ClientContext cancel_context;
  ASSERT_TRUE(skill_stub_->TerminateExecution(&cancel_context, cancel, &response).ok());
  EXPECT_TRUE(response.cancellation_requested());
}

TEST_F(GatewayServerIntegrationTest, RegisterHostRejectsShortExecutionDeadline) {
  island::eos::v1::StartExecutionRequest start;
  start.mutable_reference()->set_name("register_host");
  start.mutable_reference()->set_version("1.0.0");
  start.mutable_arguments();
  start.mutable_timeout()->set_seconds(30);
  grpc::ClientContext context;
  island::eos::v1::SkillExecution response;
  const auto status = skill_stub_->StartExecution(&context, start, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_NE(status.error_message().find("at least 5 minutes"), std::string::npos);
}

TEST_F(GatewayServerIntegrationTest, DedicatedWorkflowServiceSupportsLifecycleAndSnapshot) {
  island::eos::v1::ApplyActionListRequest apply;
  auto* definition = apply.mutable_definition();
  definition->set_workflow_id("grpc-workflow-1");
  auto* action = definition->add_actions();
  action->set_action_id("sequence");
  action->mutable_skill()->set_name("fixture.sequence");
  action->mutable_skill()->set_version("1.0.0");
  action->mutable_parameters();
  action->set_continue_on_failure(true);
  auto* bound_action = definition->add_actions();
  bound_action->set_action_id("bound");
  bound_action->mutable_skill()->set_name("fixture.pure");
  bound_action->mutable_skill()->set_version("1.0.0");
  (*bound_action->mutable_parameters()->mutable_fields())["fixture"].set_bool_value(false);
  auto* binding = bound_action->add_output_bindings();
  binding->set_source_action_id("sequence");
  binding->set_source_output_pointer("/fixture");
  binding->set_destination_parameter_pointer("/fixture");
  grpc::ClientContext apply_context;
  island::eos::v1::ActionList applied;
  const grpc::Status apply_status =
      workflow_stub_->ApplyActionList(&apply_context, apply, &applied);
  ASSERT_TRUE(apply_status.ok()) << apply_status.error_message();
  EXPECT_EQ(applied.workflow_id(), "grpc-workflow-1");
  ASSERT_EQ(applied.actions_size(), 2);
  EXPECT_TRUE(applied.actions(0).action().continue_on_failure());
  ASSERT_EQ(applied.actions(1).action().output_bindings_size(), 1);
  EXPECT_EQ(applied.actions(1).action().output_bindings(0).source_action_id(), "sequence");
  EXPECT_EQ(applied.actions(1).action().output_bindings(0).source_output_pointer(), "/fixture");
  EXPECT_EQ(applied.actions(1).action().output_bindings(0).destination_parameter_pointer(),
            "/fixture");
  ASSERT_EQ(applied.actions(1).action().depends_on_size(), 1);
  EXPECT_EQ(applied.actions(1).action().depends_on(0), "sequence");

  island::eos::v1::GetActionListRequest get;
  get.set_workflow_id("grpc-workflow-1");
  grpc::ClientContext get_context;
  island::eos::v1::ActionList fetched;
  ASSERT_TRUE(workflow_stub_->GetActionList(&get_context, get, &fetched).ok());
  EXPECT_EQ(fetched.workflow_id(), applied.workflow_id());

  grpc::ClientContext list_context;
  island::eos::v1::ListActionListsRequest list;
  island::eos::v1::ListActionListsResponse listed;
  ASSERT_TRUE(workflow_stub_->ListActionLists(&list_context, list, &listed).ok());
  EXPECT_EQ(listed.action_lists_size(), 1);

  grpc::ClientContext snapshot_context;
  island::eos::v1::GetRuntimeSnapshotRequest snapshot_request;
  island::eos::v1::ManagerRuntimeSnapshot snapshot;
  ASSERT_TRUE(
      workflow_stub_->GetRuntimeSnapshot(&snapshot_context, snapshot_request, &snapshot).ok());
  EXPECT_TRUE(snapshot.has_captured_at());
  EXPECT_EQ(snapshot.action_lists_size(), 1);
  ASSERT_TRUE(snapshot.has_action_list_cursor());
  EXPECT_FALSE(snapshot.action_list_cursor().epoch().empty());

  island::eos::v1::WatchActionListEventsRequest watch;
  watch.mutable_after()->CopyFrom(snapshot.action_list_cursor());
  grpc::ClientContext watch_context;
  auto reader = workflow_stub_->WatchActionListEvents(&watch_context, watch);
  island::eos::v1::ApplyActionListRequest second_apply;
  auto* second_definition = second_apply.mutable_definition();
  second_definition->set_workflow_id("grpc-workflow-2");
  auto* second_action = second_definition->add_actions();
  second_action->set_action_id("sequence");
  second_action->mutable_skill()->set_name("fixture.sequence");
  second_action->mutable_skill()->set_version("1.0.0");
  second_action->mutable_parameters();
  grpc::ClientContext second_context;
  island::eos::v1::ActionList second_response;
  ASSERT_TRUE(
      workflow_stub_->ApplyActionList(&second_context, second_apply, &second_response).ok());
  island::eos::v1::ActionListEvent event;
  ASSERT_TRUE(reader->Read(&event));
  EXPECT_GT(event.cursor().sequence(), snapshot.action_list_cursor().sequence());
  ASSERT_TRUE(event.has_action_list());
  EXPECT_EQ(event.action_list().workflow_id(), "grpc-workflow-2");
  watch_context.TryCancel();
  (void)reader->Finish();

  island::eos::v1::WatchActionListEventsRequest wrong_epoch;
  wrong_epoch.mutable_after()->set_epoch("not-this-boot");
  grpc::ClientContext wrong_epoch_context;
  auto wrong_epoch_reader =
      workflow_stub_->WatchActionListEvents(&wrong_epoch_context, wrong_epoch);
  EXPECT_FALSE(wrong_epoch_reader->Read(&event));
  EXPECT_EQ(wrong_epoch_reader->Finish().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(GatewayServerIntegrationTest, PublicWorkflowTemplateCatalogIsEmpty) {
  island::eos::v1::ListWorkflowTemplatesRequest list_request;
  island::eos::v1::ListWorkflowTemplatesResponse listed;
  grpc::ClientContext list_context;
  ASSERT_TRUE(workflow_stub_->ListWorkflowTemplates(&list_context, list_request, &listed).ok());
  EXPECT_EQ(listed.templates_size(), 0);

  island::eos::v1::GetWorkflowTemplateRequest get_request;
  get_request.set_name("missing");
  island::eos::v1::WorkflowTemplateDescriptor descriptor;
  grpc::ClientContext get_context;
  EXPECT_EQ(workflow_stub_->GetWorkflowTemplate(&get_context, get_request, &descriptor).error_code(),
            grpc::StatusCode::NOT_FOUND);

  island::eos::v1::RunWorkflowTemplateRequest run_request;
  run_request.set_name("missing");
  island::eos::v1::ActionList action_list;
  grpc::ClientContext run_context;
  EXPECT_EQ(workflow_stub_->RunWorkflowTemplate(&run_context, run_request, &action_list).error_code(),
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GatewayServerIntegrationTest, HealthAndReflectionAreAvailable) {
  grpc::GenericStub generic(channel_);
  grpc::CompletionQueue health_queue;
  grpc::ClientContext health_context;
  const grpc::Slice empty_slice("", 0);
  const grpc::ByteBuffer empty(&empty_slice, 1);
  grpc::ByteBuffer health_response;
  auto health = generic.PrepareUnaryCall(&health_context, "/grpc.health.v1.Health/Check", empty,
                                         &health_queue);
  ASSERT_NE(health, nullptr);
  health->StartCall();
  grpc::Status health_status;
  health->Finish(&health_response, &health_status, reinterpret_cast<void*>(1));
  void* tag = nullptr;
  bool ok = false;
  ASSERT_TRUE(health_queue.Next(&tag, &ok));
  EXPECT_TRUE(ok);
  EXPECT_EQ(tag, reinterpret_cast<void*>(1));
  EXPECT_TRUE(health_status.ok()) << health_status.error_message();
  EXPECT_EQ(BufferBytes(health_response), std::string("\x08\x01", 2));
  health_queue.Shutdown();

  grpc::CompletionQueue reflection_queue;
  grpc::ClientContext reflection_context;
  auto reflection = generic.PrepareCall(
      &reflection_context, "/grpc.reflection.v1alpha.ServerReflection/ServerReflectionInfo",
      &reflection_queue);
  ASSERT_NE(reflection, nullptr);
  reflection->StartCall(reinterpret_cast<void*>(2));
  ASSERT_TRUE(reflection_queue.Next(&tag, &ok));
  ASSERT_TRUE(ok);
  const grpc::Slice request_slice("\x3a\x00", 2);  // list_services = ""
  const grpc::ByteBuffer request_buffer(&request_slice, 1);
  reflection->Write(request_buffer, reinterpret_cast<void*>(3));
  ASSERT_TRUE(reflection_queue.Next(&tag, &ok));
  ASSERT_TRUE(ok);
  grpc::ByteBuffer reflection_response;
  reflection->Read(&reflection_response, reinterpret_cast<void*>(4));
  ASSERT_TRUE(reflection_queue.Next(&tag, &ok));
  ASSERT_TRUE(ok);
  EXPECT_NE(BufferBytes(reflection_response).find("island.eos.v1.GatewayService"),
            std::string::npos);
  EXPECT_NE(BufferBytes(reflection_response).find("island.eos.v1.SkillService"), std::string::npos);
  EXPECT_NE(BufferBytes(reflection_response).find("island.eos.v1.WorkflowService"),
            std::string::npos);
  reflection_context.TryCancel();
  grpc::Status reflection_status;
  reflection->Finish(&reflection_status, reinterpret_cast<void*>(5));
  ASSERT_TRUE(reflection_queue.Next(&tag, &ok));
  reflection_queue.Shutdown();
}

TEST_F(GatewayServerIntegrationTest, DegradedDiscoveryIsReportedOverGrpc) {
  const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds()).Halt();

  google::protobuf::Empty empty;
  island::eos::v1::DiscoverResponse discovery;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    grpc::ClientContext context;
    ASSERT_TRUE(stub_->Discover(&context, empty, &discovery).ok());
    const auto sampled = ProtoJson(discovery);
    const auto base = std::find_if(sampled.at("devices").begin(), sampled.at("devices").end(),
                                   [](const auto& device) { return device.at("id") == "base-0"; });
    if (base != sampled.at("devices").end() && base->at("health") == "unavailable") break;
    std::this_thread::sleep_for(20ms);
  } while (std::chrono::steady_clock::now() < deadline);
  const nlohmann::json grpc_discovery = ProtoJson(discovery);
  EXPECT_EQ(grpc_discovery.at("robot").at("health"), "degraded");
  const auto base =
      std::find_if(grpc_discovery.at("devices").begin(), grpc_discovery.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "base-0"; });
  ASSERT_NE(base, grpc_discovery.at("devices").end());
  EXPECT_EQ(base->at("health"), "unavailable");
}

TEST_F(GatewayServerIntegrationTest, ResourceStreamCancelsWithoutFabricatedChanges) {
  grpc::ClientContext idle_context;
  island::eos::v1::ResourceRequest idle_request;
  idle_request.set_uri("robots/demo-0/detection/state");
  auto idle_reader = stub_->WatchResource(&idle_context, idle_request);
  std::atomic<bool> idle_update{false};
  std::thread idle([&] {
    island::eos::v1::ResourceChanged update;
    idle_update.store(idle_reader->Read(&update));
    idle_reader->Finish();
  });
  std::this_thread::sleep_for(300ms);
  idle_context.TryCancel();
  idle.join();
  EXPECT_FALSE(idle_update.load());
}

TEST_F(GatewayServerIntegrationTest, GrpcMotionCancellationCanInterruptBlockingCall) {
  island::eos::v1::CallPrimitiveRequest call;
  call.set_name("gimbal.motion");
  call.set_version("1.0.0");
  call.set_device_id("gimbal-0");
  call.set_command_id("grpc-gimbal-cancel");
  auto* arguments = call.mutable_arguments()->mutable_fields();
  (*arguments)["reference"].set_string_value("absolute");
  (*arguments)["pan_rad"].set_number_value(0.4);
  (*arguments)["tilt_rad"].set_number_value(0.2);
  auto pending = std::async(std::launch::async, [this, call] {
    grpc::ClientContext context;
    google::protobuf::Struct response;
    const grpc::Status status = stub_->CallPrimitive(&context, call, &response);
    EXPECT_TRUE(status.ok()) << status.error_message();
    return response;
  });
  std::this_thread::sleep_for(50ms);
  island::eos::v1::PrimitiveCommandRequest cancel;
  cancel.set_command_id("grpc-gimbal-cancel");
  grpc::ClientContext cancel_context;
  google::protobuf::Struct cancelled;
  ASSERT_TRUE(stub_->CancelPrimitive(&cancel_context, cancel, &cancelled).ok());
  EXPECT_EQ(cancelled.fields().at("result").struct_value().fields().at("status").string_value(),
            "cancelled");
  EXPECT_EQ(ProtoJson(pending.get()), ProtoJson(cancelled));
}

}  // namespace
}  // namespace eos::runtime::northbound

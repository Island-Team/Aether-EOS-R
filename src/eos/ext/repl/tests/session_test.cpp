#include "src/eos/ext/repl/session.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "eos/gateway.grpc.pb.h"
#include "eos/skills.grpc.pb.h"
#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/north_http/client/client.h"
#include "src/eos/ext/north_http/config/config.h"
#include "src/eos/ext/north_http/server/server.h"

namespace eos::ext::repl {
namespace {

class TestGateway final : public island::eos::v1::GatewayService::Service {
 public:
  grpc::Status Discover(grpc::ServerContext*, const google::protobuf::Empty*,
                        island::eos::v1::DiscoverResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetStatus(grpc::ServerContext*, const google::protobuf::Empty*,
                         google::protobuf::Struct* response) override {
    (*response->mutable_fields())["state"].set_string_value("ready");
    return grpc::Status::OK;
  }

  grpc::Status CancelPrimitive(grpc::ServerContext*,
                               const island::eos::v1::PrimitiveCommandRequest* request,
                               google::protobuf::Struct* response) override {
    cancelled_command_id = request->command_id();
    (*response->mutable_fields())["status"].set_string_value("cancelled");
    return grpc::Status::OK;
  }

  grpc::Status ListResources(grpc::ServerContext*, const google::protobuf::Empty*,
                             google::protobuf::ListValue*) override {
    return grpc::Status::OK;
  }
  grpc::Status ReadResource(grpc::ServerContext*, const island::eos::v1::ResourceRequest*,
                             google::protobuf::Struct*) override {
    return grpc::Status::OK;
  }
  grpc::Status WatchResource(
      grpc::ServerContext* context, const island::eos::v1::ResourceRequest* request,
      grpc::ServerWriter<island::eos::v1::ResourceChanged>* writer) override {
    island::eos::v1::ResourceChanged changed;
    changed.set_uri(request->uri());
    (*changed.mutable_resource()->mutable_fields())["state"].set_string_value("ready");
    writer->Write(changed);
    while (!context->IsCancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    return {grpc::StatusCode::CANCELLED, "client disconnected"};
  }
  grpc::Status QueryResourceHistory(
      grpc::ServerContext*, const island::eos::v1::QueryResourceHistoryRequest* request,
      island::eos::v1::QueryResourceHistoryResponse* response) override {
    response->set_uri(request->uri());
    response->add_products()->set_sequence(request->sequence());
    return grpc::Status::OK;
  }
  grpc::Status CallPrimitive(grpc::ServerContext*, const island::eos::v1::CallPrimitiveRequest*,
                             google::protobuf::Struct*) override {
    return grpc::Status::OK;
  }
  grpc::Status CaptureFrames(grpc::ServerContext*, const island::eos::v1::CaptureFramesRequest*,
                              island::eos::v1::CaptureFramesResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status PutNavigationPoint(grpc::ServerContext*,
                                  const island::eos::v1::PutNavigationPointRequest* request,
                                  island::eos::v1::NavigationPoint* response) override {
    response->set_name(request->name());
    response->set_x_m(request->x_m());
    response->set_y_m(request->y_m());
    response->set_yaw_rad(request->yaw_rad());
    return grpc::Status::OK;
  }
  grpc::Status ObserveObjects(grpc::ServerContext*,
                              const island::eos::v1::ObserveObjectsRequest* request,
                              island::eos::v1::ObserveObjectsResponse* response) override {
    response->set_device_id(request->device_id());
    response->set_model_id(request->model_id());
    return grpc::Status::OK;
  }
  grpc::Status GetPerceptionRoles(grpc::ServerContext*, const google::protobuf::Empty*,
                                  island::eos::v1::PerceptionRolesResponse* response) override {
    response->add_roles()->set_role("objects");
    return grpc::Status::OK;
  }
  grpc::Status InferVad(grpc::ServerContext*, const island::eos::v1::InferVadRequest* request,
                        island::eos::v1::InferVadResponse* response) override {
    response->set_speech(!request->input().data().empty());
    return grpc::Status::OK;
  }
  grpc::Status DetectHumans(grpc::ServerContext*,
                            const island::eos::v1::DetectHumansRequest*,
                            island::eos::v1::DetectHumansResponse* response) override {
    response->set_person_count(1);
    return grpc::Status::OK;
  }
  grpc::Status EstimateHumanPose(
      grpc::ServerContext*, const island::eos::v1::EstimateHumanPoseRequest*,
      island::eos::v1::EstimateHumanPoseResponse* response) override {
    response->set_person_count(1);
    return grpc::Status::OK;
  }
  grpc::Status DetectFaces(grpc::ServerContext*, const island::eos::v1::DetectFacesRequest*,
                           island::eos::v1::DetectFacesResponse* response) override {
    response->set_status("success");
    return grpc::Status::OK;
  }

  std::string cancelled_command_id;
};

class TestSkills final : public island::eos::v1::SkillService::Service {
 public:
  grpc::Status ListSkills(grpc::ServerContext*, const island::eos::v1::ListSkillsRequest*,
                          island::eos::v1::ListSkillsResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetSkill(grpc::ServerContext*, const island::eos::v1::GetSkillRequest*,
                        island::eos::v1::SkillDescriptor*) override {
    return grpc::Status::OK;
  }
  grpc::Status StartExecution(grpc::ServerContext*, const island::eos::v1::StartExecutionRequest*,
                              island::eos::v1::SkillExecution*) override {
    return grpc::Status::OK;
  }
  grpc::Status ListExecutions(grpc::ServerContext*, const island::eos::v1::ListExecutionsRequest*,
                              island::eos::v1::ListExecutionsResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetExecution(grpc::ServerContext*, const island::eos::v1::GetExecutionRequest*,
                             island::eos::v1::SkillExecution*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetExecutionState(grpc::ServerContext*,
                                 const island::eos::v1::GetExecutionStateRequest* request,
                                 island::eos::v1::SkillExecutionState* response) override {
    response->set_execution_id(request->execution_id());
    response->set_revision(1);
    response->mutable_payload();
    return grpc::Status::OK;
  }
  grpc::Status TerminateExecution(grpc::ServerContext*, const island::eos::v1::TerminateExecutionRequest*,
                               island::eos::v1::SkillExecution*) override {
    return grpc::Status::OK;
  }
  grpc::Status TerminateExecutions(grpc::ServerContext*,
                                   const island::eos::v1::TerminateExecutionsRequest*,
                                   island::eos::v1::TerminateExecutionsResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status SignalExecution(grpc::ServerContext*, const island::eos::v1::SignalExecutionRequest*,
                               island::eos::v1::SkillSignalReceipt*) override {
    return grpc::Status::OK;
  }
  grpc::Status WatchExecution(
      grpc::ServerContext*, const island::eos::v1::WatchExecutionRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillExecution>* writer) override {
    std::uint64_t sequence;
    {
      std::lock_guard<std::mutex> lock(watch_mutex);
      watch_after.push_back(request->after_sequence());
      sequence = request->after_sequence() + 2;
    }
    if (fail_watch.load()) return grpc::Status(grpc::StatusCode::INTERNAL, "watch failed");
    island::eos::v1::SkillExecution execution;
    execution.set_execution_id(request->execution_id());
    execution.set_sequence(sequence);
    writer->Write(execution);
    return grpc::Status::OK;
  }

  grpc::Status MonitorExecutions(
      grpc::ServerContext*, const island::eos::v1::MonitorExecutionsRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillMonitorUpdate>* writer) override {
    std::uint64_t sequence;
    {
      std::lock_guard<std::mutex> lock(watch_mutex);
      monitor_after.push_back(request->after_monitor_sequence());
      monitor_names.push_back(request->selector().reference().name());
      sequence = request->after_monitor_sequence() + 1;
    }
    island::eos::v1::SkillMonitorUpdate update;
    update.set_monitor_sequence(sequence);
    writer->Write(update);
    return grpc::Status::OK;
  }

  std::mutex watch_mutex;
  std::vector<std::uint64_t> watch_after;
  std::vector<std::uint64_t> monitor_after;
  std::vector<std::string> monitor_names;
  std::atomic_bool fail_watch{false};
};

class TestWorkflows final : public island::eos::v1::WorkflowService::Service {
 public:
  grpc::Status ListWorkflowTemplates(
      grpc::ServerContext*, const island::eos::v1::ListWorkflowTemplatesRequest*,
      island::eos::v1::ListWorkflowTemplatesResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetWorkflowTemplate(
      grpc::ServerContext*, const island::eos::v1::GetWorkflowTemplateRequest*,
      island::eos::v1::WorkflowTemplateDescriptor*) override {
    return grpc::Status::OK;
  }
  grpc::Status RunWorkflowTemplate(
      grpc::ServerContext*, const island::eos::v1::RunWorkflowTemplateRequest*,
      island::eos::v1::ActionList*) override {
    return grpc::Status::OK;
  }
  grpc::Status ApplyActionList(grpc::ServerContext*, const island::eos::v1::ApplyActionListRequest*,
                               island::eos::v1::ActionList*) override {
    return grpc::Status::OK;
  }
  grpc::Status ListActionLists(grpc::ServerContext*, const island::eos::v1::ListActionListsRequest*,
                               island::eos::v1::ListActionListsResponse*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetActionList(grpc::ServerContext*, const island::eos::v1::GetActionListRequest*,
                             island::eos::v1::ActionList*) override {
    return grpc::Status::OK;
  }
  grpc::Status GetRuntimeSnapshot(grpc::ServerContext*,
                                  const island::eos::v1::GetRuntimeSnapshotRequest*,
                                  island::eos::v1::ManagerRuntimeSnapshot*) override {
    return grpc::Status::OK;
  }
};

class SessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &grpc_port_);
    builder.RegisterService(&service_);
    builder.RegisterService(&skills_);
    builder.RegisterService(&workflows_);
    grpc_server_ = builder.BuildAndStart();
    ASSERT_NE(grpc_server_, nullptr);

    north_http::Config config;
    config.port = 0;
    config.upstream = "127.0.0.1:" + std::to_string(grpc_port_);
    config.connect_timeout_ms = 1000;
    config.request_timeout_ms = 5000;
    client_ = std::make_unique<north_http::Client>(config.upstream);
    adapter_ = std::make_unique<north_http::Server>(std::move(config), *client_);
    adapter_thread_ = std::thread([this] { adapter_->Run([this] { return stop_.load(); }); });
    for (int i = 0; i < 100 && !adapter_->listening(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_TRUE(adapter_->listening());
    std::string error;
    ASSERT_TRUE(session_.Connect({"127.0.0.1", adapter_->port()}, 2000, &error)) << error;
  }

  void TearDown() override {
    session_.Close();
    stop_.store(true);
    if (adapter_thread_.joinable()) adapter_thread_.join();
    adapter_.reset();
    grpc_server_->Shutdown();
  }

  TestGateway service_;
  TestSkills skills_;
  TestWorkflows workflows_;
  int grpc_port_ = 0;
  std::unique_ptr<grpc::Server> grpc_server_;
  std::atomic_bool stop_{false};
  std::unique_ptr<north_http::Client> client_;
  std::unique_ptr<north_http::Server> adapter_;
  std::thread adapter_thread_;
  Session session_;
};

TEST_F(SessionTest, CancelsPrimitiveThroughHttpAdapter) {
  const nlohmann::json result =
      session_.Call("CancelPrimitive", {{"command_id", "command-17"}}, 5000);
  EXPECT_EQ(result.at("status"), "cancelled");
  EXPECT_EQ(service_.cancelled_command_id, "command-17");
}

TEST_F(SessionTest, ReadsAuditStatusThroughHttpAdapter) {
  const nlohmann::json result = session_.Call("GetAuditStatus", nullptr, 5000);
  EXPECT_EQ(result.at("enabled"), false);
  EXPECT_EQ(result.at("health"), "disabled");
}

TEST_F(SessionTest, PerceptionRequestsRoundTripThroughHttpAdapter) {
  const nlohmann::json observation = session_.Call(
      "ObserveObjects", {{"robotName", "demo-0"}, {"deviceId", "camera-0"},
                         {"modelId", "objects"}}, 5000);
  EXPECT_EQ(observation.at("deviceId"), "camera-0");
  EXPECT_EQ(observation.at("modelId"), "objects");
  const nlohmann::json roles = session_.Call("GetPerceptionRoles", nullptr, 5000);
  ASSERT_TRUE(roles.at("roles").is_array());
  ASSERT_EQ(roles.at("roles").size(), 1u);
  EXPECT_EQ(roles.at("roles").at(0).at("role"), "objects");
}

TEST_F(SessionTest, NavigationPointPutSendsJsonBody) {
  const nlohmann::json point = session_.Call(
      "PutNavigationPoint", {{"robot_name", "demo-0"}, {"name", "entry"},
                             {"x_m", 1.25}, {"y_m", -2.0}, {"yaw_rad", 0.5}}, 5000);
  EXPECT_EQ(point.at("name"), "entry");
  EXPECT_DOUBLE_EQ(point.at("xM").get<double>(), 1.25);
  EXPECT_DOUBLE_EQ(point.at("yM").get<double>(), -2.0);
  EXPECT_DOUBLE_EQ(point.at("yawRad").get<double>(), 0.5);
}

TEST_F(SessionTest, MapsEveryUnaryCommandToTheAdapterSurface) {
  const std::vector<std::pair<std::string, nlohmann::json>> calls = {
      {"Discover", nullptr},
      {"GetStatus", nullptr},
      {"ListResources", nullptr},
      {"ReadResource", {{"uri", "robots/r/state"}}},
      {"QueryResourceHistory", {{"uri", "robots/r/state"}, {"sequence", "7"}}},
      {"CallPrimitive", {{"name", "p"}, {"arguments", nlohmann::json::object()}}},
      {"CaptureFrames", {{"device_id", "camera"}}},
      {"ObserveObjects", {{"robotName", "demo-0"}, {"deviceId", "camera-0"},
                          {"modelId", "objects"}}},
      {"GetPerceptionRoles", nullptr},
      {"InferVad", {{"modelId", "vad"}, {"input", {{"data", "UklGRg=="}}}}},
      {"DetectHumans", {{"modelId", "human"}, {"input", {{"data", "/9j/2Q=="}}}}},
      {"EstimateHumanPose",
       {{"modelId", "pose"},
        {"input", {{"data", "/9j/2Q=="}}},
        {"persons", nlohmann::json::array()}}},
      {"DetectFaces", {{"modelId", "face"}, {"input", {{"data", "/9j/2Q=="}}}}},
      {"ListSkills", nullptr},
      {"GetSkill", {{"reference", {{"name", "s"}, {"version", "1"}}}}},
      {"StartExecution",
       {{"reference", {{"name", "s"}, {"version", "1"}}},
        {"arguments", nlohmann::json::object()},
        {"timeout", "1s"}}},
      {"ListExecutions", nullptr},
      {"GetExecution", {{"execution_id", "e"}}},
      {"GetExecutionState", {{"execution_id", "e"}}},
      {"TerminateExecution", {{"execution_id", "e"}}},
      {"TerminateExecutions",
       {{"reference", {{"name", "s"}}}, {"request_id", "pkill-1"}}},
      {"SignalExecution",
       {{"execution_id", "e"},
        {"request_id", "r"},
        {"name", "go"},
        {"payload", nlohmann::json::object()}}},
      {"ListWorkflowTemplates", nullptr},
      {"GetWorkflowTemplate", {{"name", "register_host"}}},
      {"RunWorkflowTemplate",
       {{"name", "register_host"}, {"workflow_id", "debug-register-host"}}},
      {"ApplyActionList", {{"definition", {{"workflowId", "w"}}}}},
      {"ListActionLists", nullptr},
      {"GetActionList", {{"workflow_id", "w"}}},
      {"GetRuntimeSnapshot", nullptr},
  };
  for (const auto& [method, params] : calls) {
    const nlohmann::json result = session_.Call(method, params, 5000);
    EXPECT_FALSE(result.contains("error")) << method << ": " << result.dump();
  }
}

TEST_F(SessionTest, MapsEveryResourceHistorySelectorThroughStrictProtoJson) {
  const std::vector<nlohmann::json> queries = {
      {{"uri", "robots/r/state"}, {"sequence", "7"}},
      {{"uri", "robots/r/state"}, {"before", "2026-10-04T12:00:00Z"}},
      {{"uri", "robots/r/state"}, {"after", "2026-10-04T12:00:00Z"}},
      {{"uri", "robots/r/state"}, {"nearest", "2026-10-04T12:00:00Z"}},
      {{"uri", "robots/r/state"},
       {"sequenceRange", {{"firstSequence", "4"}, {"lastSequence", "9"}, {"limit", 3}}}},
  };
  for (const auto& query : queries) {
    const nlohmann::json result = session_.Call("QueryResourceHistory", query, 5000);
    EXPECT_FALSE(result.contains("error")) << query.dump() << ": " << result.dump();
  }
}

TEST_F(SessionTest, WrapsAdapterErrorsConsistently) {
  const nlohmann::json result = session_.Call("ReadResource", nlohmann::json::object(), 5000);
  EXPECT_FALSE(result.contains("error"));
  const nlohmann::json unknown = session_.Call("NoSuchMethod", nullptr, 5000);
  ASSERT_TRUE(unknown.contains("error"));
  EXPECT_EQ(unknown.at("error").at("code"), 12);
}

TEST_F(SessionTest, ResourceWatchDeliversSnapshotsAndCancelsPromptly) {
  std::mutex mutex;
  std::condition_variable changed;
  nlohmann::json resource;
  std::string error;
  ASSERT_TRUE(session_.Watch(
      "robot/state",
      [&](const nlohmann::json& value) {
        std::lock_guard<std::mutex> lock(mutex);
        resource = value;
        changed.notify_all();
      },
      &error))
      << error;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return !resource.empty(); }));
  }
  EXPECT_EQ(resource.at("state"), "ready");

  const auto started = std::chrono::steady_clock::now();
  session_.Unwatch("robot/state");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
}

TEST_F(SessionTest, ReconnectsExecutionStreamsFromTheLastEventId) {
  std::mutex mutex;
  std::condition_variable changed;
  int updates = 0;
  std::string error;
  ASSERT_TRUE(session_.WatchStream(
      "execution:e", "/v1/eos/executions/e:watch?afterSequence=5",
       [&](const std::string&, const nlohmann::json&) {
         std::lock_guard<std::mutex> lock(mutex);
         ++updates;
         changed.notify_all();
         return true;
       },
      &error))
      << error;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return updates >= 2; }));
  }
  session_.Unwatch("execution:e");
  std::lock_guard<std::mutex> lock(skills_.watch_mutex);
  ASSERT_GE(skills_.watch_after.size(), 2u);
  EXPECT_EQ(skills_.watch_after[0], 5u);
  EXPECT_EQ(skills_.watch_after[1], 7u);
}

TEST_F(SessionTest, ReconnectKeepsMonitorFiltersAndAdvancesCursor) {
  std::mutex mutex;
  std::condition_variable changed;
  int updates = 0;
  std::string error;
  ASSERT_TRUE(session_.WatchStream(
      "ps", "/v1/eos/executions:monitor?skillName=inspect&afterMonitorSequence=5",
      [&](const std::string&, const nlohmann::json&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++updates;
        changed.notify_all();
        return true;
      },
      &error)) << error;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return updates >= 2; }));
  }
  session_.Unwatch("ps");
  std::lock_guard<std::mutex> lock(skills_.watch_mutex);
  ASSERT_GE(skills_.monitor_after.size(), 2u);
  EXPECT_EQ(skills_.monitor_after[0], 5u);
  EXPECT_EQ(skills_.monitor_after[1], 6u);
  EXPECT_EQ(skills_.monitor_names[0], "inspect");
  EXPECT_EQ(skills_.monitor_names[1], "inspect");
}

TEST_F(SessionTest, StopsExecutionStreamWhenHandlerReturnsFalse) {
  std::mutex mutex;
  std::condition_variable changed;
  int updates = 0;
  std::string error;
  ASSERT_TRUE(session_.WatchStream(
      "execution:e", "/v1/eos/executions/e:watch",
      [&](const std::string&, const nlohmann::json&) {
        std::lock_guard<std::mutex> lock(mutex);
        ++updates;
        changed.notify_all();
        return false;
      },
      &error))
      << error;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return updates == 1; }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  session_.Unwatch("execution:e");
  std::lock_guard<std::mutex> lock(skills_.watch_mutex);
  EXPECT_EQ(skills_.watch_after.size(), 1u);
}

TEST_F(SessionTest, StopsExecutionStreamAfterServerErrorEvent) {
  skills_.fail_watch.store(true);
  std::mutex mutex;
  std::condition_variable changed;
  int errors = 0;
  std::string error;
  ASSERT_TRUE(session_.WatchStream(
      "execution:e", "/v1/eos/executions/e:watch",
      [&](const std::string& event, const nlohmann::json&) {
        if (event == "error") {
          std::lock_guard<std::mutex> lock(mutex);
          ++errors;
          changed.notify_all();
        }
        return true;
      },
      &error))
      << error;
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(changed.wait_for(lock, std::chrono::seconds(3), [&] { return errors == 1; }));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  session_.Unwatch("execution:e");
  std::lock_guard<std::mutex> lock(skills_.watch_mutex);
  EXPECT_EQ(skills_.watch_after.size(), 1u);
}

TEST_F(SessionTest, ReceivesEventBufferedWithSseHeaders) {
  namespace asio = boost::asio;
  using tcp = asio::ip::tcp;
  asio::io_context io;
  tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
  acceptor.non_blocking(true);
  const auto port = acceptor.local_endpoint().port();
  std::atomic_bool stop_server{false};
  std::thread server([&] {
    for (int request = 0; request < 2 && !stop_server.load();) {
      tcp::socket socket(io);
      boost::system::error_code accept_error;
      acceptor.accept(socket, accept_error);
      if (accept_error == asio::error::would_block || accept_error == asio::error::try_again) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      if (accept_error) break;
      asio::streambuf input;
      asio::read_until(socket, input, "\r\n\r\n");
      const std::string response = request == 0
                                       ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}"
                                       : "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
                                         "event: error\ndata: {\"code\":13,\"message\":\"boom\"}\n\n";
      asio::write(socket, asio::buffer(response));
      if (request == 1) std::this_thread::sleep_for(std::chrono::milliseconds(300));
      ++request;
    }
  });

  std::string error;
  const bool connected = session_.Connect({"127.0.0.1", port}, 2000, &error);
  std::mutex mutex;
  std::condition_variable changed;
  bool received = false;
  bool watching = false;
  if (connected) {
    watching = session_.WatchStream(
        "coalesced", "/events",
        [&](const std::string& event, const nlohmann::json& payload) {
          std::lock_guard<std::mutex> lock(mutex);
          received = event == "error" && payload.value("message", "") == "boom";
          changed.notify_all();
          return false;
        },
        &error);
  }
  if (watching) {
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_for(lock, std::chrono::seconds(2), [&] { return received; });
  }
  session_.Unwatch("coalesced");
  stop_server.store(true);
  server.join();
  ASSERT_TRUE(connected) << error;
  ASSERT_TRUE(watching) << error;
  EXPECT_TRUE(received);
}

TEST_F(SessionTest, RejectsExcessWatches) {
  std::string error;
  ASSERT_TRUE(session_.WatchStream("watch-0", "/v1/eos/executions/e:watch",
                                   [](const std::string&, const nlohmann::json&) { return true; },
                                   &error));
  for (int i = 1; i < 16; ++i) {
    ASSERT_TRUE(session_.WatchStream(
        "watch-" + std::to_string(i), "/v1/eos/executions/e:watch",
        [](const std::string&, const nlohmann::json&) { return true; }, &error));
  }
  EXPECT_FALSE(session_.WatchStream("watch-16", "/v1/eos/executions/e:watch",
                                    [](const std::string&, const nlohmann::json&) { return true; },
                                    &error));
  EXPECT_EQ(error, "watch limit reached");
}

TEST_F(SessionTest, ContainsWatchHandlerExceptions) {
  std::mutex mutex;
  std::condition_variable called;
  bool entered = false;
  std::string error;
  ASSERT_TRUE(session_.WatchStream(
      "throwing", "/v1/eos/executions/e:watch",
      [&](const std::string&, const nlohmann::json&) -> bool {
        {
          std::lock_guard<std::mutex> lock(mutex);
          entered = true;
        }
        called.notify_all();
        throw std::runtime_error("handler failed");
      },
      &error));
  std::unique_lock<std::mutex> lock(mutex);
  EXPECT_TRUE(called.wait_for(lock, std::chrono::seconds(3), [&] { return entered; }));
  lock.unlock();
  session_.Unwatch("throwing");
}

}  // namespace
}  // namespace eos::ext::repl

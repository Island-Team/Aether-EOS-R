#include "src/eos/ext/north_http/server/server.h"

#include <google/protobuf/empty.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "eos/gateway.grpc.pb.h"
#include "eos/skills.grpc.pb.h"
#include "eos/workflow.grpc.pb.h"
#include "src/eos/ext/north_http/audit/audit.h"
#include "src/eos/ext/north_http/client/client.h"

namespace eos::ext::north_http {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

class Gate {
 public:
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait_for(lock, 10s, [this] { return released_; });
  }

  bool WaitUntilEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, 2s, [this] { return entered_; });
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{false};
  bool released_{false};
};

class RequestRecorder {
 public:
  void Record(grpc::ServerContext* context) {
    const auto found = context->client_metadata().find("x-request-id");
    std::lock_guard<std::mutex> lock(mutex_);
    request_id_ = found == context->client_metadata().end()
                      ? ""
                      : std::string(found->second.data(), found->second.size());
  }
  std::string request_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return request_id_;
  }

 private:
  mutable std::mutex mutex_;
  std::string request_id_;
};

class FakeGateway final : public island::eos::v1::GatewayService::Service {
 public:
  explicit FakeGateway(RequestRecorder& recorder) : recorder_(recorder) {}

  grpc::Status Discover(grpc::ServerContext* context, const google::protobuf::Empty*,
                        island::eos::v1::DiscoverResponse* response) override {
    recorder_.Record(context);
    response->mutable_robot()->set_id("robot-1");
    return grpc::Status::OK;
  }
  grpc::Status GetStatus(grpc::ServerContext* context, const google::protobuf::Empty*,
                         google::protobuf::Struct* response) override {
    recorder_.Record(context);
    const auto found = context->client_metadata().find("x-request-id");
    const std::string request_id = found == context->client_metadata().end()
                                       ? ""
                                       : std::string(found->second.data(), found->second.size());
    if (request_id == "slow-request") {
      slow_started.store(true);
      std::this_thread::sleep_for(250ms);
    }
    if (request_id == "hold-unary") unary_gate.Wait();
    (*response->mutable_fields())["status"].set_string_value("ready");
    return grpc::Status::OK;
  }
  grpc::Status ListResources(grpc::ServerContext* context, const google::protobuf::Empty*,
                             google::protobuf::ListValue* response) override {
    recorder_.Record(context);
    response->add_values()->set_string_value("robots/robot-1/state");
    return grpc::Status::OK;
  }
  grpc::Status ReadResource(grpc::ServerContext* context,
                            const island::eos::v1::ResourceRequest* request,
                            google::protobuf::Struct* response) override {
    recorder_.Record(context);
    context->AddInitialMetadata("eos-sample-age-ms", "42");
    (*response->mutable_fields())["uri"].set_string_value(request->uri());
    return grpc::Status::OK;
  }
  grpc::Status ListNavigationPoints(
      grpc::ServerContext* context, const island::eos::v1::ListNavigationPointsRequest* request,
      island::eos::v1::ListNavigationPointsResponse* response) override {
    recorder_.Record(context);
    auto* point = response->add_points();
    point->set_name(request->robot_name());
    point->set_x_m(1.0);
    return grpc::Status::OK;
  }
  grpc::Status GetNavigationPoint(
      grpc::ServerContext* context, const island::eos::v1::GetNavigationPointRequest* request,
      island::eos::v1::NavigationPoint* response) override {
    recorder_.Record(context);
    response->set_name(request->name());
    return grpc::Status::OK;
  }
  grpc::Status PutNavigationPoint(
      grpc::ServerContext* context, const island::eos::v1::PutNavigationPointRequest* request,
      island::eos::v1::NavigationPoint* response) override {
    recorder_.Record(context);
    *response = island::eos::v1::NavigationPoint();
    response->set_name(request->name());
    response->set_x_m(request->x_m());
    response->set_y_m(request->y_m());
    response->set_yaw_rad(request->yaw_rad());
    return grpc::Status::OK;
  }
  grpc::Status DeleteNavigationPoint(
      grpc::ServerContext* context, const island::eos::v1::GetNavigationPointRequest*,
      google::protobuf::Empty*) override {
    recorder_.Record(context);
    return grpc::Status::OK;
  }
  grpc::Status ReloadNavigationPoints(
      grpc::ServerContext* context, const google::protobuf::Empty*,
      island::eos::v1::ReloadNavigationPointsResponse* response) override {
    recorder_.Record(context);
    response->set_point_count(10);
    return grpc::Status::OK;
  }
  grpc::Status QueryResourceHistory(
      grpc::ServerContext* context, const island::eos::v1::QueryResourceHistoryRequest* request,
      island::eos::v1::QueryResourceHistoryResponse* response) override {
    recorder_.Record(context);
    response->set_uri(request->uri());
    response->set_schema_uri("island://schemas/test.v1.json");
    auto* product = response->add_products();
    product->set_sequence(request->sequence());
    product->mutable_observed_at()->set_seconds(1);
    product->set_boot_id("boot-1");
    product->set_epoch("epoch-1");
    (*product->mutable_value()->mutable_fields())["state"].set_string_value("ready");
    response->mutable_retention()->set_first_available_sequence(1);
    response->mutable_retention()->set_latest_sequence(1);
    return grpc::Status::OK;
  }
  grpc::Status WatchResource(
      grpc::ServerContext* context, const island::eos::v1::ResourceRequest* request,
      grpc::ServerWriter<island::eos::v1::ResourceChanged>* writer) override {
    recorder_.Record(context);
    if (request->uri() == "hold-sse") sse_gate.Wait();
    if (request->uri() == "delayed") std::this_thread::sleep_for(150ms);
    island::eos::v1::ResourceChanged changed;
    changed.set_uri(request->uri());
    (*changed.mutable_resource()->mutable_fields())["state"].set_string_value("ready");
    writer->Write(changed);
    if (request->uri() == "wait-for-cancel") {
      while (!context->IsCancelled()) std::this_thread::sleep_for(2ms);
      watch_cancelled.store(true);
      return {grpc::StatusCode::CANCELLED, "client disconnected"};
    }
    return grpc::Status::OK;
  }
  grpc::Status CallPrimitive(grpc::ServerContext* context,
                             const island::eos::v1::CallPrimitiveRequest* request,
                             google::protobuf::Struct* response) override {
    recorder_.Record(context);
    (*response->mutable_fields())["commandId"].set_string_value(request->command_id());
    return grpc::Status::OK;
  }
  grpc::Status CancelPrimitive(grpc::ServerContext* context,
                               const island::eos::v1::PrimitiveCommandRequest* request,
                               google::protobuf::Struct* response) override {
    recorder_.Record(context);
    (*response->mutable_fields())["commandId"].set_string_value(request->command_id());
    return grpc::Status::OK;
  }
  grpc::Status CaptureFrames(grpc::ServerContext* context,
                             const island::eos::v1::CaptureFramesRequest* request,
                             island::eos::v1::CaptureFramesResponse* response) override {
    recorder_.Record(context);
    response->set_device_id(request->device_id());
    return grpc::Status::OK;
  }
  grpc::Status ObserveObjects(grpc::ServerContext* context,
                              const island::eos::v1::ObserveObjectsRequest* request,
                              island::eos::v1::ObserveObjectsResponse* response) override {
    recorder_.Record(context);
    response->set_device_id(request->device_id());
    response->set_model_id(request->model_id());
    response->set_source_timestamp_ns(request->source_timestamp_ns());
    response->set_jpeg("\xff\xd8\xff\xd9");
    return grpc::Status::OK;
  }
  grpc::Status InferVad(grpc::ServerContext* context,
                        const island::eos::v1::InferVadRequest* request,
                        island::eos::v1::InferVadResponse* response) override {
    recorder_.Record(context);
    response->set_speech(!request->input().data().empty());
    response->set_probability(request->threshold());
    return grpc::Status::OK;
  }
  grpc::Status DetectHumans(grpc::ServerContext* context,
                            const island::eos::v1::DetectHumansRequest* request,
                            island::eos::v1::DetectHumansResponse* response) override {
    recorder_.Record(context);
    auto* person = response->add_persons();
    person->set_track_id(7);
    person->set_confidence(request->confidence_threshold());
    response->set_person_count(1);
    return grpc::Status::OK;
  }
  grpc::Status EstimateHumanPose(
      grpc::ServerContext* context, const island::eos::v1::EstimateHumanPoseRequest* request,
      island::eos::v1::EstimateHumanPoseResponse* response) override {
    recorder_.Record(context);
    auto* person = response->add_persons();
    person->set_track_id(request->persons(0).track_id());
    for (int i = 0; i < 17; ++i) {
      person->add_keypoints()->set_x(i);
      person->add_keypoint_scores(0.9);
    }
    person->set_posture("standing");
    response->set_person_count(1);
    return grpc::Status::OK;
  }
  grpc::Status DetectFaces(grpc::ServerContext* context,
                           const island::eos::v1::DetectFacesRequest*,
                           island::eos::v1::DetectFacesResponse* response) override {
    recorder_.Record(context);
    response->set_status("success");
    response->set_operation("recognize");
    auto* face = response->mutable_data()->add_faces();
    face->set_identity("Ada");
    face->set_known(true);
    face->set_similarity(0.98);
    face->set_direction_px(12.0);
    response->mutable_data()->set_face_count(1);
    return grpc::Status::OK;
  }
  grpc::Status GetPerceptionRoles(
      grpc::ServerContext* context, const google::protobuf::Empty*,
      island::eos::v1::PerceptionRolesResponse* response) override {
    recorder_.Record(context);
    auto* role = response->add_roles();
    role->set_role("human");
    role->set_model_id("human-model");
    role->set_available(true);
    role->set_continuous(true);
    response->set_camera_device_id("camera-1");
    return grpc::Status::OK;
  }
  grpc::Status WatchMonitor(
      grpc::ServerContext* context, const island::eos::v1::MonitorRequest* request,
      grpc::ServerWriter<island::eos::v1::MonitorEvent>* writer) override {
    recorder_.Record(context);
    if (request->camera_device_id() == "hold-monitor") monitor_gate.Wait();
    island::eos::v1::MonitorEvent event;
    auto* ready = event.mutable_ready();
    ready->set_camera_device_id(request->camera_device_id());
    ready->set_microphone_device_id(request->microphone_device_id());
    ready->set_camera_width_px(1920);
    ready->set_camera_height_px(1080);
    ready->set_camera_fps(request->max_camera_fps());
    ready->set_audio_sample_rate_hz(16000);
    ready->set_audio_channels(1);
    ready->set_detection_status(request->camera_device_id() == "camera-1"
                                    ? "available" : "not_configured");
    writer->Write(event);
    if (!request->microphone_device_id().empty()) {
      event.Clear();
      auto* audio = event.mutable_audio();
      audio->set_rms(0.25);
      audio->set_peak(0.5);
      audio->set_dbfs(-12.0);
      audio->set_sequence(7);
      audio->set_sample_rate_hz(16000);
      audio->set_channels(1);
      audio->set_captured_at("2026-10-03T05:40:11.000Z");
      writer->Write(event);
    }
    event.Clear();
    auto* camera = event.mutable_camera();
    camera->set_jpeg(std::string("\xff\xd8\xff\xd9", 4));
    camera->set_width_px(1920);
    camera->set_height_px(1080);
    camera->set_sequence(9);
    camera->set_captured_at("2026-10-03T05:40:11.001Z");
    camera->set_dropped_frames(2);
    writer->Write(event);
    if (request->include_detections() &&
        request->camera_device_id() == "camera-1") {
      if (request->detection_source() == "object") {
        event.Clear();
        auto* detection = event.mutable_detection();
        detection->set_jpeg(std::string("\xff\xd8\x02\xff\xd9", 5));
        detection->set_width_px(640);
        detection->set_height_px(480);
        detection->set_sequence(12);
        detection->set_generation(5);
        detection->set_detection_json(
            R"({"type":"detection.snapshot","detections":[{"kind":"object","label":"paper_ball","bbox":[10,20,30,40]}]})");
        writer->Write(event);
        return grpc::Status::OK;
      }
      for (int enrichment = 0; enrichment < 2; ++enrichment) {
        event.Clear();
        auto* detection = event.mutable_detection();
        detection->set_jpeg(std::string("\xff\xd8\x01\xff\xd9", 5));
        detection->set_width_px(640);
        detection->set_height_px(480);
        detection->set_sequence(11 + enrichment);
        detection->set_generation(4);
        detection->set_detection_json(
            enrichment == 0
                ? R"({"type":"detection.snapshot","models":{"pose":{"status":"pending"}},"detections":[]})"
                : R"({"type":"detection.snapshot","models":{"pose":{"status":"complete"}},"detections":[]})");
        writer->Write(event);
      }
    }
    return grpc::Status::OK;
  }

  std::atomic_bool slow_started{false};
  std::atomic_bool watch_cancelled{false};
  Gate unary_gate;
  Gate sse_gate;
  Gate monitor_gate;

  void ReleaseGates() {
    unary_gate.Release();
    sse_gate.Release();
    monitor_gate.Release();
  }

 private:
  RequestRecorder& recorder_;
};

void FillExecution(const std::string& id, island::eos::v1::SkillExecution* response) {
  response->set_execution_id(id);
  response->set_sequence(7);
  response->set_latest_journal_sequence(9);
  response->set_status(island::eos::v1::EXECUTION_STATUS_RUNNING);
}

class FakeSkills final : public island::eos::v1::SkillService::Service {
 public:
  explicit FakeSkills(RequestRecorder& recorder) : recorder_(recorder) {}

  grpc::Status ListSkills(grpc::ServerContext* context, const island::eos::v1::ListSkillsRequest*,
                          island::eos::v1::ListSkillsResponse* response) override {
    recorder_.Record(context);
    response->add_skills()->mutable_reference()->set_name("demo.skill");
    return grpc::Status::OK;
  }
  grpc::Status GetSkill(grpc::ServerContext* context,
                        const island::eos::v1::GetSkillRequest* request,
                        island::eos::v1::SkillDescriptor* response) override {
    recorder_.Record(context);
    *response->mutable_reference() = request->reference();
    return grpc::Status::OK;
  }
  grpc::Status StartExecution(grpc::ServerContext* context,
                              const island::eos::v1::StartExecutionRequest*,
                              island::eos::v1::SkillExecution* response) override {
    recorder_.Record(context);
    FillExecution("execution-1", response);
    return grpc::Status::OK;
  }
  grpc::Status GetExecution(grpc::ServerContext* context,
                            const island::eos::v1::GetExecutionRequest* request,
                            island::eos::v1::SkillExecution* response) override {
    recorder_.Record(context);
    FillExecution(request->execution_id(), response);
    return grpc::Status::OK;
  }
  grpc::Status WatchExecution(
      grpc::ServerContext* context, const island::eos::v1::WatchExecutionRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillExecution>* writer) override {
    recorder_.Record(context);
    island::eos::v1::SkillExecution execution;
    FillExecution(request->execution_id(), &execution);
    execution.set_sequence(request->after_sequence() + 1);
    writer->Write(execution);
    return grpc::Status::OK;
  }
  grpc::Status GetExecutionState(
      grpc::ServerContext* context, const island::eos::v1::GetExecutionStateRequest* request,
      island::eos::v1::SkillExecutionState* response) override {
    recorder_.Record(context);
    response->set_execution_id(request->execution_id());
    response->set_revision(3);
    response->mutable_published_at()->set_seconds(10);
    (*response->mutable_payload()->mutable_fields())["phase"].set_string_value("tracking");
    return grpc::Status::OK;
  }
  grpc::Status WatchExecutionState(
      grpc::ServerContext* context, const island::eos::v1::WatchExecutionStateRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillExecutionState>* writer) override {
    recorder_.Record(context);
    if (request->execution_id() == "delayed") std::this_thread::sleep_for(150ms);
    island::eos::v1::SkillExecutionState state;
    state.set_execution_id(request->execution_id());
    state.set_revision(request->after_revision() + 1);
    state.mutable_payload();
    writer->Write(state);
    return grpc::Status::OK;
  }
  grpc::Status TerminateExecution(grpc::ServerContext* context,
                               const island::eos::v1::TerminateExecutionRequest* request,
                               island::eos::v1::SkillExecution* response) override {
    recorder_.Record(context);
    FillExecution(request->execution_id(), response);
    response->set_cancellation_requested(true);
    return grpc::Status::OK;
  }
  grpc::Status TerminateExecutions(
      grpc::ServerContext* context, const island::eos::v1::TerminateExecutionsRequest* request,
      island::eos::v1::TerminateExecutionsResponse* response) override {
    recorder_.Record(context);
    response->set_request_id(request->request_id());
    response->add_execution_ids("execution-1");
    return grpc::Status::OK;
  }
  grpc::Status ListExecutions(grpc::ServerContext* context,
                              const island::eos::v1::ListExecutionsRequest*,
                              island::eos::v1::ListExecutionsResponse* response) override {
    recorder_.Record(context);
    FillExecution("execution-1", response->add_executions());
    return grpc::Status::OK;
  }
  grpc::Status SignalExecution(grpc::ServerContext* context,
                               const island::eos::v1::SignalExecutionRequest* request,
                               island::eos::v1::SkillSignalReceipt* response) override {
    recorder_.Record(context);
    response->set_execution_id(request->execution_id());
    response->set_accepted(true);
    return grpc::Status::OK;
  }
  grpc::Status FollowExecutionJournal(
      grpc::ServerContext* context, const island::eos::v1::FollowExecutionJournalRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillJournalEntry>* writer) override {
    recorder_.Record(context);
    island::eos::v1::SkillJournalEntry event;
    event.set_execution_id(request->execution_id());
    event.set_journal_sequence(request->after_journal_sequence() + 1);
    writer->Write(event);
    return grpc::Status::OK;
  }
  grpc::Status MonitorExecutions(
      grpc::ServerContext* context, const island::eos::v1::MonitorExecutionsRequest* request,
      grpc::ServerWriter<island::eos::v1::SkillMonitorUpdate>* writer) override {
    recorder_.Record(context);
    island::eos::v1::SkillMonitorUpdate update;
    update.set_monitor_sequence(request->after_monitor_sequence() + 1);
    FillExecution("execution-1", update.add_upserted());
    writer->Write(update);
    return grpc::Status::OK;
  }

 private:
  RequestRecorder& recorder_;
};

class FakeWorkflows final : public island::eos::v1::WorkflowService::Service {
 public:
  explicit FakeWorkflows(RequestRecorder& recorder) : recorder_(recorder) {}

  grpc::Status ListWorkflowTemplates(
      grpc::ServerContext* context, const island::eos::v1::ListWorkflowTemplatesRequest*,
      island::eos::v1::ListWorkflowTemplatesResponse* response) override {
    recorder_.Record(context);
    response->add_templates()->set_name("register_host");
    return grpc::Status::OK;
  }
  grpc::Status GetWorkflowTemplate(
      grpc::ServerContext* context, const island::eos::v1::GetWorkflowTemplateRequest* request,
      island::eos::v1::WorkflowTemplateDescriptor* response) override {
    recorder_.Record(context);
    response->set_name(request->name());
    return grpc::Status::OK;
  }
  grpc::Status RunWorkflowTemplate(
      grpc::ServerContext* context, const island::eos::v1::RunWorkflowTemplateRequest* request,
      island::eos::v1::ActionList* response) override {
    recorder_.Record(context);
    response->set_workflow_id(request->workflow_id());
    return grpc::Status::OK;
  }

  grpc::Status ApplyActionList(grpc::ServerContext* context,
                               const island::eos::v1::ApplyActionListRequest* request,
                               island::eos::v1::ActionList* response) override {
    recorder_.Record(context);
    response->set_workflow_id(request->definition().workflow_id());
    return grpc::Status::OK;
  }
  grpc::Status GetActionList(grpc::ServerContext* context,
                             const island::eos::v1::GetActionListRequest* request,
                             island::eos::v1::ActionList* response) override {
    recorder_.Record(context);
    response->set_workflow_id(request->workflow_id());
    return grpc::Status::OK;
  }
  grpc::Status ListActionLists(grpc::ServerContext* context,
                               const island::eos::v1::ListActionListsRequest*,
                               island::eos::v1::ListActionListsResponse* response) override {
    recorder_.Record(context);
    response->add_action_lists()->set_workflow_id("workflow-1");
    return grpc::Status::OK;
  }
  grpc::Status GetRuntimeSnapshot(grpc::ServerContext* context,
                                   const island::eos::v1::GetRuntimeSnapshotRequest*,
                                   island::eos::v1::ManagerRuntimeSnapshot* response) override {
    recorder_.Record(context);
    const auto found = context->client_metadata().find("x-request-id");
    const bool force_json_error =
        found != context->client_metadata().end() &&
        std::string(found->second.data(), found->second.size()) == "worker-exception";
    response->mutable_captured_at()->set_seconds(force_json_error ? 253402300800 : 1);
    return grpc::Status::OK;
  }
  grpc::Status WatchActionListEvents(
      grpc::ServerContext* context, const island::eos::v1::WatchActionListEventsRequest* request,
      grpc::ServerWriter<island::eos::v1::ActionListEvent>* writer) override {
    recorder_.Record(context);
    island::eos::v1::ActionListEvent event;
    event.mutable_cursor()->set_epoch(request->after().epoch());
    event.mutable_cursor()->set_sequence(request->after().sequence() + 1);
    event.mutable_action_list()->set_workflow_id("workflow-1");
    writer->Write(event);
    return grpc::Status::OK;
  }

 private:
  RequestRecorder& recorder_;
};

struct HttpResult {
  unsigned status;
  std::string body;
  std::string request_id;
  std::string allow_origin;
  std::string cache_control;
  std::string sample_age_ms;
  std::string exposed_headers;
};

class NorthHttpServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&gateway_);
    builder.RegisterService(&skills_);
    builder.RegisterService(&workflows_);
    grpc_server_ = builder.BuildAndStart();
    ASSERT_NE(grpc_server_, nullptr);

    upstream_ = "127.0.0.1:" + std::to_string(port);
    StartNorthHttp(TestConfig());
  }

  Config TestConfig() const {
    Config config;
    config.port = 0;
    config.upstream = upstream_;
    config.max_request_bytes = 1024;
    config.request_timeout_ms = 100;
    config.sse_heartbeat_ms = 50;
    config.max_inflight_requests = 1;
    return config;
  }

  Config CapacityConfig() const {
    Config config = TestConfig();
    config.max_connections = 3;
    config.max_inflight_requests = 1;
    config.max_sse_streams = 1;
    config.max_monitor_streams = 1;
    config.request_timeout_ms = 10000;
    return config;
  }

  void StartNorthHttp(const Config& config) {
    client_ = std::make_unique<Client>(config.upstream);
    north_http_ = std::make_unique<Server>(config, *client_);
    north_http_thread_ = std::thread([this] { north_http_->Run([this] { return stop_.load(); }); });
    for (int i = 0; i < 500 && !north_http_->listening(); ++i) std::this_thread::sleep_for(2ms);
    ASSERT_TRUE(north_http_->listening());
  }

  void RestartNorthHttp(const Config& config) {
    stop_.store(true);
    if (north_http_thread_.joinable()) north_http_thread_.join();
    north_http_.reset();
    client_.reset();
    stop_.store(false);
    StartNorthHttp(config);
  }

  void TearDown() override {
    gateway_.ReleaseGates();
    stop_.store(true);
    if (north_http_thread_.joinable()) north_http_thread_.join();
    if (grpc_server_) grpc_server_->Shutdown();
  }

  HttpResult Request(http::verb method, const std::string& target, const std::string& body = {},
                     const std::string& request_id = {}, const std::string& origin = {},
                     bool duplicate_request_id = false) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    http::request<http::string_body> request(method, target, 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::content_type, "application/json");
    if (!request_id.empty()) request.set("X-Request-ID", request_id);
    if (duplicate_request_id) request.insert("X-Request-ID", "duplicate-trace");
    if (!origin.empty()) request.set(http::field::origin, origin);
    request.keep_alive(false);
    request.body() = body;
    request.prepare_payload();
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return {response.result_int(), response.body(), std::string(response["X-Request-ID"]),
            std::string(response[http::field::access_control_allow_origin]),
            std::string(response[http::field::cache_control]),
            std::string(response["Eos-Sample-Age-Ms"]),
            std::string(response[http::field::access_control_expose_headers])};
  }

  std::string Sse(const std::string& target, const std::string& last_event_id = {},
                  const std::string& origin = {}) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    std::string request = "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n";
    if (!last_event_id.empty()) request += "Last-Event-ID: " + last_event_id + "\r\n";
    if (!origin.empty()) request += "Origin: " + origin + "\r\n";
    request += "\r\n";
    asio::write(socket, asio::buffer(request));
    std::string response;
    boost::system::error_code error;
    for (;;) {
      char bytes[1024];
      const std::size_t size = socket.read_some(asio::buffer(bytes), error);
      response.append(bytes, size);
      if (error == asio::error::eof) break;
      if (error) break;
    }
    return response;
  }

  std::vector<std::pair<bool, std::string>> Monitor(const std::string& subscription) {
    asio::io_context io;
    websocket::stream<tcp::socket> socket(io);
    socket.next_layer().connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    socket.handshake("127.0.0.1", "/v1/eos/monitor/ws");
    socket.write(asio::buffer(subscription));
    std::vector<std::pair<bool, std::string>> messages;
    boost::system::error_code error;
    for (;;) {
      beast::flat_buffer buffer;
      socket.read(buffer, error);
      if (error == websocket::error::closed) break;
      EXPECT_FALSE(error) << error.message();
      if (error) break;
      messages.emplace_back(socket.got_text(), beast::buffers_to_string(buffer.data()));
    }
    return messages;
  }

  HttpResult WebSocketUpgrade() {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.expires_after(2s);
    stream.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    http::request<http::empty_body> request(http::verb::get, "/v1/eos/monitor/ws", 11);
    request.set(http::field::host, "127.0.0.1");
    request.set(http::field::upgrade, "websocket");
    request.set(http::field::connection, "upgrade");
    request.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
    request.set(http::field::sec_websocket_version, "13");
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return {response.result_int(), response.body(), std::string(response["X-Request-ID"]), {}, {}};
  }

  bool RequestEndsWithoutResponse(const std::string& request_id) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.expires_after(2s);
    stream.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    http::request<http::empty_body> request(http::verb::get, "/v1/eos/runtime:snapshot", 11);
    request.set(http::field::host, "127.0.0.1");
    request.set("X-Request-ID", request_id);
    request.keep_alive(false);
    http::write(stream, request);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code error;
    http::read(stream, buffer, response, error);
    return static_cast<bool>(error);
  }

  RequestRecorder recorder_;
  FakeGateway gateway_{recorder_};
  FakeSkills skills_{recorder_};
  FakeWorkflows workflows_{recorder_};
  std::unique_ptr<grpc::Server> grpc_server_;
  std::unique_ptr<Client> client_;
  std::unique_ptr<Server> north_http_;
  std::atomic_bool stop_{false};
  std::thread north_http_thread_;
  std::string upstream_;
};

TEST_F(NorthHttpServerTest, HealthCorsAndRequestIdsCrossTheBoundary) {
  EXPECT_EQ(Request(http::verb::get, "/health/live").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/health/ready").status, 200u);
  const auto response =
      Request(http::verb::get, "/v1/eos/status", {}, "trace-123", "http://localhost:3000");
  EXPECT_EQ(response.status, 200u);
  EXPECT_EQ(response.request_id, "trace-123");
  EXPECT_EQ(response.allow_origin, "http://localhost:3000");
  EXPECT_EQ(recorder_.request_id(), "trace-123");
  const auto invalid = Request(http::verb::get, "/v1/eos/status", {}, "bad request id");
  EXPECT_EQ(invalid.status, 400u);
  EXPECT_NE(invalid.body.find("requestId"), std::string::npos);
  const auto duplicate = Request(http::verb::get, "/v1/eos/status", {}, "trace-1", {}, true);
  EXPECT_EQ(duplicate.status, 400u);
  EXPECT_NE(duplicate.body.find("at most once"), std::string::npos);
  EXPECT_TRUE(Request(http::verb::get, "/v1/eos/status", {}, {}, "https://evil.example")
                  .allow_origin.empty());
  const auto audit_status = Request(http::verb::get, "/v1/eos/audit/status");
  EXPECT_EQ(audit_status.status, 200u);
  EXPECT_NE(audit_status.body.find("disabled"), std::string::npos);
  EXPECT_EQ(audit_status.cache_control, "no-store");
  const auto audit_records = Request(http::verb::get, "/v1/eos/audit/action-lists");
  EXPECT_EQ(audit_records.status, 200u);
  EXPECT_NE(audit_records.body.find("records"), std::string::npos);
  EXPECT_NE(audit_records.body.find("\"truncated\":false"), std::string::npos);
  EXPECT_EQ(audit_records.cache_control, "no-store");
}

TEST_F(NorthHttpServerTest, GatewayUnaryRoutesUseStrictProtoJson) {
  EXPECT_NE(Request(http::verb::get, "/v1/eos/discover").body.find("robot-1"), std::string::npos);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/resources").status, 200u);
  const auto resource = Request(http::verb::get, "/v1/eos/resource?uri=robot%2Fstate", {}, {},
                                "http://localhost:3000");
  EXPECT_NE(resource.body.find("robot/state"), std::string::npos);
  EXPECT_EQ(resource.sample_age_ms, "42");
  EXPECT_NE(resource.exposed_headers.find("Eos-Sample-Age-Ms"), std::string::npos);
  const auto history = Request(http::verb::post, "/v1/eos/resource:history",
                               R"({"uri":"robot/state","sequence":"1"})");
  EXPECT_EQ(history.status, 200u);
  EXPECT_NE(history.body.find("island://schemas/test.v1.json"), std::string::npos);
  EXPECT_NE(history.body.find("\"sequence\":\"1\""), std::string::npos);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/primitives:call",
                    R"({"name":"base.move","commandId":"command-1"})")
                .status,
            200u);
  EXPECT_EQ(
      Request(http::verb::post, "/v1/eos/motions:cancel", R"({"commandId":"command-1"})").status,
      200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/world/camera:capture", R"({"deviceId":"camera-1"})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/motions:cancel", R"({"unknown":true})").status,
            400u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/motions:cancel", "{").status, 400u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/does-not-exist").status, 404u);
  const auto oversized = Request(http::verb::post, "/v1/eos/motions:cancel", std::string(2048, 'x'),
                                 "oversized-trace");
  EXPECT_EQ(oversized.status, 413u);
  EXPECT_NE(oversized.body.find("requestId"), std::string::npos);
  EXPECT_EQ(oversized.request_id, "oversized-trace");
  EXPECT_NE(oversized.body.find("oversized-trace"), std::string::npos);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/executions/execution-1:watch-junk").status, 404u);
}

TEST_F(NorthHttpServerTest, NavigationPointRoutesUseTypedMethodsAndPathAuthority) {
  const std::string collection = "/v1/eos/robots/demo-0/navigation/waypoints";
  EXPECT_NE(Request(http::verb::get, collection).body.find("demo-0"), std::string::npos);
  const std::string item = collection + "/Entry%20in";
  EXPECT_NE(Request(http::verb::get, item).body.find("Entry in"), std::string::npos);
  const auto put = Request(http::verb::put, item,
                           R"({"xM":1.25,"yM":-2,"yawRad":0.5})");
  EXPECT_EQ(put.status, 200u);
  EXPECT_NE(put.body.find("1.25"), std::string::npos);
  EXPECT_EQ(Request(http::verb::put, item, R"({"xM":1})").status, 400u);
  EXPECT_EQ(Request(http::verb::put, item,
                    R"({"name":"different","xM":1,"yM":2,"yawRad":3})")
                .status,
            400u);
  EXPECT_EQ(Request(http::verb::delete_, item).status, 200u);
  EXPECT_NE(Request(http::verb::post, "/v1/eos/navigation/waypoints:reload", "{}").body.find("10"),
            std::string::npos);
}

TEST_F(NorthHttpServerTest, TypedPerceptionRoutesUseStrictProtoJson) {
  const auto objects = Request(
      http::verb::post, "/v1/eos/perception/objects:observe",
      R"({"robotName":"demo","deviceId":"camera-1","modelId":"detector","sourceTimestampNs":"1791436400123456789"})");
  EXPECT_EQ(objects.status, 200u);
  EXPECT_NE(objects.body.find("\"deviceId\":\"camera-1\""), std::string::npos);
  EXPECT_NE(objects.body.find("\"sourceTimestampNs\":\"1791436400123456789\""),
            std::string::npos);
  EXPECT_NE(objects.body.find("\"jpeg\":\"/9j/2Q==\""), std::string::npos);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/perception/objects:observe",
                    R"({"deviceId":"camera-1","unknown":true})").status,
            400u);

  const auto vad = Request(http::verb::post, "/v1/eos/perception/vad:infer",
                           R"({"modelId":"vad","input":{"data":"UklGRg=="},"threshold":0.6})");
  EXPECT_EQ(vad.status, 200u);
  EXPECT_NE(vad.body.find("\"speech\":true"), std::string::npos);
  EXPECT_NE(vad.body.find("0.6"), std::string::npos);

  const auto humans = Request(
      http::verb::post, "/v1/eos/perception/humans:detect",
      R"({"modelId":"human","input":{"data":"/9j/2Q==","mediaType":"image/jpeg"},"confidenceThreshold":0.7})");
  EXPECT_EQ(humans.status, 200u);
  EXPECT_NE(humans.body.find("\"trackId\":\"7\""), std::string::npos);

  const auto pose = Request(
      http::verb::post, "/v1/eos/perception/human-poses:estimate",
      R"({"modelId":"pose","input":{"data":"/9j/2Q=="},"persons":[{"trackId":"7","bbox":{"x1":1,"y1":2,"x2":3,"y2":4},"confidence":0.9}]})");
  EXPECT_EQ(pose.status, 200u);
  EXPECT_NE(pose.body.find("standing"), std::string::npos);

  const auto faces = Request(http::verb::post, "/v1/eos/perception/faces:detect",
                             R"({"modelId":"face","input":{"data":"/9j/2Q=="}})");
  EXPECT_EQ(faces.status, 200u);
  EXPECT_NE(faces.body.find("Ada"), std::string::npos);
  EXPECT_NE(faces.body.find("directionPx"), std::string::npos);

  EXPECT_EQ(Request(http::verb::post, "/v1/eos/perception/vad:infer",
                    R"({"modelId":"vad","input":{"data":"UklGRg=="},"unknown":true})")
                .status,
            400u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/perception/vad:infer").status, 404u);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketTranslatesTextAndBinaryEvents) {
  const auto messages = Monitor(
      R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","microphone_device_id":"microphone-1"})");
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_TRUE(messages[0].first);
  EXPECT_NE(messages[0].second.find("monitor.ready"), std::string::npos);
  EXPECT_NE(messages[0].second.find("1920"), std::string::npos);
  EXPECT_TRUE(messages[1].first);
  EXPECT_NE(messages[1].second.find("monitor.audio"), std::string::npos);
  ASSERT_FALSE(messages[2].first);
  const std::string& frame = messages[2].second;
  ASSERT_GT(frame.size(), 16u);
  EXPECT_EQ(frame.substr(0, 4), "EOSM");
  EXPECT_EQ(static_cast<unsigned char>(frame[4]), 1u);
  EXPECT_EQ(static_cast<unsigned char>(frame[5]), 1u);
  EXPECT_EQ(static_cast<unsigned char>(frame[6]), 0u);
  EXPECT_EQ(static_cast<unsigned char>(frame[7]), 0u);
  const std::uint32_t metadata_size =
      (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[8])) << 24) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[9])) << 16) |
      (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[10])) << 8) |
      static_cast<std::uint32_t>(static_cast<unsigned char>(frame[11]));
  ASSERT_LE(12u + metadata_size + 4u, frame.size());
  const std::string metadata = frame.substr(12, metadata_size);
  EXPECT_NE(metadata.find("\"sequence\":\"9\""), std::string::npos);
  EXPECT_NE(metadata.find("\"dropped_frames\":\"2\""), std::string::npos);
  EXPECT_EQ(frame.substr(12 + metadata_size), std::string("\xff\xd8\xff\xd9", 4));
}

TEST_F(NorthHttpServerTest, ExposesConfiguredPerceptionRoles) {
  const auto response = Request(http::verb::get, "/v1/eos/perception/roles");
  EXPECT_EQ(response.status, 200u);
  EXPECT_NE(response.body.find("human-model"), std::string::npos);
  EXPECT_NE(response.body.find("camera-1"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketRejectsMalformedSubscription) {
  const auto messages = Monitor(R"({"version":1,"type":"monitor.subscribe"})");
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_TRUE(messages[0].first);
  EXPECT_NE(messages[0].second.find("monitor.error"), std::string::npos);
  EXPECT_NE(messages[0].second.find("invalid_subscription"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketV1SupportsCameraOnlyAndExactDetectionFrames) {
  const auto messages = Monitor(
      R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","include_detections":true})");
  ASSERT_EQ(messages.size(), 4u);
  EXPECT_TRUE(messages[0].first);
  EXPECT_NE(messages[0].second.find("\"detection_status\":\"available\""), std::string::npos);
  ASSERT_FALSE(messages[2].first);
  ASSERT_FALSE(messages[3].first);
  for (std::size_t index : {2u, 3u}) {
    const std::string& frame = messages[index].second;
    ASSERT_GT(frame.size(), 17u);
    EXPECT_EQ(frame.substr(0, 4), "EOSM");
    EXPECT_EQ(static_cast<unsigned char>(frame[4]), 1u);
    EXPECT_EQ(static_cast<unsigned char>(frame[5]), 2u);
    const std::uint32_t metadata_size =
        (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[8])) << 24) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[9])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(frame[10])) << 8) |
        static_cast<std::uint32_t>(static_cast<unsigned char>(frame[11]));
    const std::string metadata = frame.substr(12, metadata_size);
    EXPECT_NE(metadata.find("\"generation\":\"4\""), std::string::npos);
    EXPECT_EQ(frame.substr(12 + metadata_size), std::string("\xff\xd8\x01\xff\xd9", 5));
  }
  EXPECT_NE(messages[2].second.find("pending"), std::string::npos);
  EXPECT_NE(messages[3].second.find("complete"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketV1ForwardsObjectDetectionSource) {
  const auto messages = Monitor(
      R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","include_detections":true,"detection_source":"object"})");
  ASSERT_EQ(messages.size(), 3u);
  EXPECT_NE(messages[0].second.find("\"detection_status\":\"available\""),
            std::string::npos);
  ASSERT_FALSE(messages[2].first);
  EXPECT_NE(messages[2].second.find("paper_ball"), std::string::npos);
  EXPECT_NE(messages[2].second.find("[10,20,30,40]"), std::string::npos);
  EXPECT_EQ(messages[2].second.substr(messages[2].second.size() - 5),
            std::string("\xff\xd8\x02\xff\xd9", 5));
  const auto invalid = Monitor(
      R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","detection_source":"unknown"})");
  ASSERT_EQ(invalid.size(), 1u);
  EXPECT_NE(invalid[0].second.find("invalid_subscription"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketV1ReportsUnconfiguredCameraAndRejectsOtherVersions) {
  const auto raw = Monitor(
      R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-2","include_detections":true})");
  ASSERT_FALSE(raw.empty());
  EXPECT_NE(raw[0].second.find("\"detection_status\":\"not_configured\""), std::string::npos);
  const auto invalid = Monitor(
      R"({"version":2,"type":"monitor.subscribe","camera_device_id":"camera-1","microphone_device_id":"microphone-1","include_detections":true})");
  ASSERT_EQ(invalid.size(), 1u);
  EXPECT_NE(invalid[0].second.find("invalid_subscription"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorWebSocketRejectsDisallowedOriginBeforeUpgrade) {
  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
  http::request<http::empty_body> request(http::verb::get, "/v1/eos/monitor/ws", 11);
  request.set(http::field::host, "127.0.0.1");
  request.set(http::field::upgrade, "websocket");
  request.set(http::field::connection, "upgrade");
  request.set(http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
  request.set(http::field::sec_websocket_version, "13");
  request.set(http::field::origin, "https://evil.example");
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);
  EXPECT_EQ(response.result(), http::status::forbidden);
}

TEST_F(NorthHttpServerTest, UpstreamDeadlineMapsToGatewayTimeout) {
  const auto response = Request(http::verb::get, "/v1/eos/status", {}, "slow-request");
  EXPECT_EQ(response.status, 504u);
  EXPECT_NE(response.body.find("slow-request"), std::string::npos);
}

TEST_F(NorthHttpServerTest, RejectsRequestsAboveTheActiveBound) {
  auto slow = std::async(std::launch::async, [this] {
    return Request(http::verb::get, "/v1/eos/status", {}, "slow-request");
  });
  for (int i = 0; i < 500 && !gateway_.slow_started.load(); ++i) std::this_thread::sleep_for(2ms);
  ASSERT_TRUE(gateway_.slow_started.load());
  const auto rejected = Request(http::verb::get, "/v1/eos/discover");
  EXPECT_EQ(rejected.status, 429u);
  EXPECT_NE(rejected.body.find("too many active"), std::string::npos);
  EXPECT_EQ(slow.get().status, 504u);
}

TEST_F(NorthHttpServerTest, RejectsOverloadedUnaryRequestFromHeadersWithoutWaitingForBody) {
  RestartNorthHttp(CapacityConfig());
  auto unary = std::async(std::launch::async, [this] {
    return Request(http::verb::get, "/v1/eos/status", {}, "hold-unary");
  });

  const bool unary_held = gateway_.unary_gate.WaitUntilEntered();
  EXPECT_TRUE(unary_held);
  if (unary_held) {
    asio::io_context io;
    tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
    const std::string request =
        "POST /v1/eos/motions:cancel HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n\r\n";
    asio::write(socket, asio::buffer(request));

    std::string response;
    boost::system::error_code read_error;
    bool read_finished = false;
    asio::async_read(socket, asio::dynamic_buffer(response), asio::transfer_all(),
                     [&](const boost::system::error_code& error, std::size_t) {
                       read_error = error;
                       read_finished = true;
                     });
    io.run_for(1s);

    gateway_.unary_gate.Release();
    if (!read_finished) {
      boost::system::error_code ignored;
      socket.cancel(ignored);
      socket.close(ignored);
      io.restart();
      io.run();
    }

    EXPECT_TRUE(read_finished);
    EXPECT_EQ(read_error, asio::error::eof);
    EXPECT_NE(response.find("HTTP/1.1 429 Too Many Requests"), std::string::npos);
    EXPECT_NE(response.find("too many active unary north HTTP requests"), std::string::npos);
  }

  gateway_.ReleaseGates();
  const auto unary_status = unary.wait_for(2s);
  EXPECT_EQ(unary_status, std::future_status::ready);
  if (unary_status == std::future_status::ready) EXPECT_EQ(unary.get().status, 200u);
}

TEST_F(NorthHttpServerTest, HardConnectionLimitQueuesAndReusesWorkerSlot) {
  RestartNorthHttp(CapacityConfig());
  auto unary = std::async(std::launch::async, [this] {
    return Request(http::verb::get, "/v1/eos/status", {}, "hold-unary");
  });
  auto sse = std::async(std::launch::async, [this] {
    return Sse("/v1/eos/resource:watch?uri=hold-sse");
  });
  auto monitor = std::async(std::launch::async, [this] {
    return Monitor(
        R"({"version":1,"type":"monitor.subscribe","camera_device_id":"hold-monitor","microphone_device_id":"microphone-1"})");
  });

  const bool all_workers_held = gateway_.unary_gate.WaitUntilEntered() &&
                                gateway_.sse_gate.WaitUntilEntered() &&
                                gateway_.monitor_gate.WaitUntilEntered();
  EXPECT_TRUE(all_workers_held);
  if (all_workers_held) {
    auto queued = std::async(std::launch::async, [this] {
      return Request(http::verb::get, "/v1/eos/discover");
    });
    EXPECT_EQ(queued.wait_for(150ms), std::future_status::timeout);
    gateway_.unary_gate.Release();
    const auto queued_status = queued.wait_for(2s);
    EXPECT_EQ(queued_status, std::future_status::ready);
    if (queued_status == std::future_status::ready) EXPECT_EQ(queued.get().status, 200u);
  }

  gateway_.ReleaseGates();
  const auto unary_status = unary.wait_for(2s);
  const auto sse_status = sse.wait_for(2s);
  const auto monitor_status = monitor.wait_for(2s);
  EXPECT_EQ(unary_status, std::future_status::ready);
  EXPECT_EQ(sse_status, std::future_status::ready);
  EXPECT_EQ(monitor_status, std::future_status::ready);
  if (unary_status == std::future_status::ready) EXPECT_EQ(unary.get().status, 200u);
  if (sse_status == std::future_status::ready)
    EXPECT_NE(sse.get().find("event: resource-changed"), std::string::npos);
  if (monitor_status == std::future_status::ready) EXPECT_EQ(monitor.get().size(), 3u);
}

TEST_F(NorthHttpServerTest, UnaryAndSseLimitsAreIndependentAndClassSpecific) {
  RestartNorthHttp(CapacityConfig());
  auto unary = std::async(std::launch::async, [this] {
    return Request(http::verb::get, "/v1/eos/status", {}, "hold-unary");
  });
  auto sse = std::async(std::launch::async, [this] {
    return Sse("/v1/eos/resource:watch?uri=hold-sse");
  });

  const bool unary_held = gateway_.unary_gate.WaitUntilEntered();
  const bool sse_held = gateway_.sse_gate.WaitUntilEntered();
  EXPECT_TRUE(unary_held);
  EXPECT_TRUE(sse_held);
  if (unary_held && sse_held) {
    const auto rejected_unary = Request(http::verb::get, "/v1/eos/discover");
    EXPECT_EQ(rejected_unary.status, 429u);
    EXPECT_NE(rejected_unary.body.find("too many active unary north HTTP requests"),
              std::string::npos);
    const std::string rejected_sse = Sse("/v1/eos/resource:watch?uri=second");
    EXPECT_NE(rejected_sse.find("429 Too Many Requests"), std::string::npos);
    EXPECT_NE(rejected_sse.find("too many active north HTTP SSE streams"), std::string::npos);
  }

  gateway_.ReleaseGates();
  const auto unary_status = unary.wait_for(2s);
  const auto sse_status = sse.wait_for(2s);
  EXPECT_EQ(unary_status, std::future_status::ready);
  EXPECT_EQ(sse_status, std::future_status::ready);
  if (unary_status == std::future_status::ready) EXPECT_EQ(unary.get().status, 200u);
  if (sse_status == std::future_status::ready)
    EXPECT_NE(sse.get().find("event: resource-changed"), std::string::npos);
}

TEST_F(NorthHttpServerTest, MonitorLimitRejectsAndThenReusesCapacity) {
  RestartNorthHttp(CapacityConfig());
  auto held = std::async(std::launch::async, [this] {
    return Monitor(
        R"({"version":1,"type":"monitor.subscribe","camera_device_id":"hold-monitor","microphone_device_id":"microphone-1"})");
  });
  const bool monitor_held = gateway_.monitor_gate.WaitUntilEntered();
  EXPECT_TRUE(monitor_held);
  if (monitor_held) {
    const auto rejected = WebSocketUpgrade();
    EXPECT_EQ(rejected.status, 429u);
    EXPECT_NE(rejected.body.find("too many active north HTTP monitor streams"),
              std::string::npos);
  }

  gateway_.monitor_gate.Release();
  const auto held_status = held.wait_for(2s);
  EXPECT_EQ(held_status, std::future_status::ready);
  if (held_status == std::future_status::ready) {
    EXPECT_EQ(held.get().size(), 3u);
    EXPECT_EQ(Monitor(
                  R"({"version":1,"type":"monitor.subscribe","camera_device_id":"camera-1","microphone_device_id":"microphone-1"})")
                  .size(),
              3u);
  }
}

TEST_F(NorthHttpServerTest, WorkerExceptionIsContainedAndSlotIsReusable) {
  RestartNorthHttp(CapacityConfig());
  EXPECT_TRUE(RequestEndsWithoutResponse("worker-exception"));
  auto reused = std::async(std::launch::async, [this] {
    return Request(http::verb::get, "/v1/eos/discover");
  });
  const auto reused_status = reused.wait_for(2s);
  EXPECT_EQ(reused_status, std::future_status::ready);
  if (reused_status == std::future_status::ready) EXPECT_EQ(reused.get().status, 200u);
}

TEST_F(NorthHttpServerTest, SkillAndWorkflowUnaryRoutesAreComplete) {
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/skills").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/skills/demo.skill/versions/1.0.0").status, 200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/skills/demo.skill/versions/1.0.0/executions",
                     R"({"arguments":{},"timeout":"1s","requestId":"start-1"})")
                 .status,
             200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/skills/demo.skill/versions/1.0.0/executions",
                    R"({"arguments":{},"requestId":"start-no-deadline"})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/executions").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/executions/execution-1").status, 200u);
  const auto state = Request(http::verb::get, "/v1/eos/executions/execution-1/state");
  EXPECT_EQ(state.status, 200u);
  EXPECT_NE(state.body.find("tracking"), std::string::npos);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/executions/execution-1:terminate").status, 200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/executions:terminate",
                    R"({"reference":{"name":"demo.skill"},"requestId":"pkill-1"})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/executions/execution-1:signal",
                    R"({"requestId":"signal-1","name":"continue","payload":{}})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/workflow-templates").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/workflow-templates/register_host").status, 200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/workflow-templates/register_host:run",
                    R"({"workflowId":"debug-register-host"})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::post, "/v1/eos/workflows",
                    R"({"definition":{"workflowId":"workflow-1"}})")
                .status,
            200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/workflows").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/workflows/workflow-1").status, 200u);
  EXPECT_EQ(Request(http::verb::get, "/v1/eos/runtime:snapshot").status, 200u);
}

TEST_F(NorthHttpServerTest, StreamsUseTypedEventsAndSequenceIds) {
  const std::string resource = Sse("/v1/eos/resource:watch?uri=robot%2Fstate");
  EXPECT_NE(resource.find("Content-Type: text/event-stream"), std::string::npos);
  EXPECT_NE(resource.find("event: resource-changed"), std::string::npos);
  EXPECT_NE(resource.find("robot/state"), std::string::npos);
  EXPECT_NE(resource.find("ready"), std::string::npos);
  const std::string cors_resource =
      Sse("/v1/eos/resource:watch?uri=robot%2Fstate", {}, "http://localhost:3000");
  EXPECT_NE(cors_resource.find("Access-Control-Expose-Headers: X-Request-ID"), std::string::npos);

  const std::string execution = Sse("/v1/eos/executions/execution-1:watch?afterSequence=11", "6");
  EXPECT_NE(execution.find("event: skill-execution"), std::string::npos);
  EXPECT_NE(execution.find("id: 7"), std::string::npos);

  const std::string state =
      Sse("/v1/eos/executions/execution-1/state:watch?afterRevision=8", "3");
  EXPECT_NE(state.find("event: skill-execution-state"), std::string::npos);
  EXPECT_NE(state.find("id: 4"), std::string::npos);

  const std::string journal =
      Sse("/v1/eos/executions/execution-1/journal:follow?afterJournalSequence=13", "8");
  EXPECT_NE(journal.find("event: skill-journal-entry"), std::string::npos);
  EXPECT_NE(journal.find("id: 9"), std::string::npos);

  const std::string monitor =
      Sse("/v1/eos/executions:monitor?skillName=demo.skill&status=RUNNING&afterMonitorSequence=5",
          "2");
  EXPECT_NE(monitor.find("event: skill-monitor-update"), std::string::npos);
  EXPECT_NE(monitor.find("id: 3"), std::string::npos);

  const std::string workflows = Sse("/v1/eos/workflows:watch?cursor=boot-1%3A21");
  EXPECT_NE(workflows.find("event: action-list-event"), std::string::npos);
  EXPECT_NE(workflows.find("id: boot-1:22"), std::string::npos);
  EXPECT_NE(workflows.find("workflow-1"), std::string::npos);

  const std::string workflow_resume = Sse("/v1/eos/workflows:watch", "boot-2:30");
  EXPECT_NE(workflow_resume.find("id: boot-2:31"), std::string::npos);
  const std::string workflow_header_precedence =
      Sse("/v1/eos/workflows:watch?cursor=boot-1%3A21", "boot-2:30");
  EXPECT_NE(workflow_header_precedence.find("id: boot-2:31"), std::string::npos);
  const std::string invalid = Sse("/v1/eos/workflows:watch?cursor=invalid");
  EXPECT_NE(invalid.find("event: error"), std::string::npos);
}

TEST_F(NorthHttpServerTest, ResourceAndExecutionStateStreamsOutliveUnaryDeadline) {
  const std::string resource = Sse("/v1/eos/resource:watch?uri=delayed");
  EXPECT_NE(resource.find("event: resource-changed"), std::string::npos);
  EXPECT_EQ(resource.find("DEADLINE_EXCEEDED"), std::string::npos);

  const std::string state = Sse("/v1/eos/executions/delayed/state:watch?afterRevision=0");
  EXPECT_NE(state.find("event: skill-execution-state"), std::string::npos);
  EXPECT_EQ(state.find("DEADLINE_EXCEEDED"), std::string::npos);
}

TEST_F(NorthHttpServerTest, ClientDisconnectCancelsTheUpstreamStream) {
  asio::io_context io;
  tcp::socket socket(io);
  socket.connect({asio::ip::make_address("127.0.0.1"), north_http_->port()});
  const std::string request =
      "GET /v1/eos/resource:watch?uri=wait-for-cancel HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";
  asio::write(socket, asio::buffer(request));
  std::string response;
  for (int i = 0; i < 50 && response.find("resource-changed") == std::string::npos; ++i) {
    char bytes[1024];
    boost::system::error_code error;
    const std::size_t size = socket.read_some(asio::buffer(bytes), error);
    response.append(bytes, size);
    ASSERT_FALSE(error && error != asio::error::eof) << error.message();
  }
  ASSERT_NE(response.find("resource-changed"), std::string::npos);
  socket.close();
  for (int i = 0; i < 500 && !gateway_.watch_cancelled.load(); ++i)
    std::this_thread::sleep_for(2ms);
  EXPECT_TRUE(gateway_.watch_cancelled.load());
}

TEST(NorthHttpUnavailableTest, LivenessDoesNotDependOnUpstreamReadiness) {
  Config config;
  config.port = 0;
  config.upstream = "127.0.0.1:1";
  config.connect_timeout_ms = 20;
  Client client(config.upstream);
  Server server(config, client);
  std::atomic_bool stop{false};
  std::thread thread([&] { server.Run([&] { return stop.load(); }); });
  for (int i = 0; i < 500 && !server.listening(); ++i) std::this_thread::sleep_for(2ms);
  ASSERT_TRUE(server.listening());
  auto request = [&](const std::string& target) {
    asio::io_context io;
    beast::tcp_stream stream(io);
    stream.connect({asio::ip::make_address("127.0.0.1"), server.port()});
    http::request<http::empty_body> message(http::verb::get, target, 11);
    message.set(http::field::host, "127.0.0.1");
    message.keep_alive(false);
    http::write(stream, message);
    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    http::read(stream, buffer, response);
    return response.result_int();
  };
  EXPECT_EQ(request("/health/live"), 200);
  EXPECT_EQ(request("/health/ready"), 503);
  EXPECT_EQ(request("/v1/eos/status"), 503);
  stop.store(true);
  thread.join();
}

// Optional diagnostics remain visible without gating the functional HTTP adapter.
TEST(NorthHttpReadinessTest, ReportsEnabledDegradedAuditSeparatelyFromHealthyUpstream) {
  grpc::EnableDefaultHealthCheckService(true);
  grpc::ServerBuilder builder;
  int grpc_port = 0;
  RequestRecorder recorder;
  FakeGateway gateway(recorder);
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &grpc_port);
  builder.RegisterService(&gateway);
  auto grpc_server = builder.BuildAndStart();
  ASSERT_NE(grpc_server, nullptr);

  Config config;
  config.port = 0;
  config.upstream = "127.0.0.1:" + std::to_string(grpc_port);
  Client client(config.upstream);
  audit::Service audit(client, audit::Config{});
  audit.Start();
  ASSERT_EQ(audit.status().health, audit::Health::kDegraded);
  Server server(config, client, &audit);
  std::atomic_bool stop{false};
  std::thread thread([&] { server.Run([&] { return stop.load(); }); });
  for (int i = 0; i < 500 && !server.listening(); ++i) std::this_thread::sleep_for(2ms);
  ASSERT_TRUE(server.listening());

  asio::io_context io;
  beast::tcp_stream stream(io);
  stream.connect({asio::ip::make_address("127.0.0.1"), server.port()});
  http::request<http::empty_body> request(http::verb::get, "/health/ready", 11);
  request.set(http::field::host, "127.0.0.1");
  request.keep_alive(false);
  http::write(stream, request);
  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  http::read(stream, buffer, response);
  EXPECT_EQ(response.result_int(), 200);
  EXPECT_NE(response.body().find("ready"), std::string::npos);

  stop.store(true);
  thread.join();
  audit.Stop();
  grpc_server->Shutdown();
}

}  // namespace
}  // namespace eos::ext::north_http

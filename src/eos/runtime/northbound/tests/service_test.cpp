#include "src/eos/runtime/northbound/service.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <thread>

#include "src/eos/ext/simulator/map_store.h"
#include "src/eos/lib/clock.h"
#include "src/eos/lib/log.h"
#include "src/eos/runtime/dispatch/dispatcher.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/hardware/telemetry_hub.h"
#include "src/eos/runtime/init/schemas.h"
#include "src/eos/runtime/models/microphone_vad.h"
#include "src/eos/runtime/models/runtime.h"
#include "src/eos/runtime/northbound/log_status.h"
#include "src/eos/runtime/northbound/subscribe.h"
#include "src/eos/runtime/perception/body/reader.h"
#include "src/eos/runtime/perception/product_history.h"
#include "src/eos/runtime/perception/resource_catalog.h"
#include "src/eos/runtime/store/navigation_point_store.h"
#include "tests/eos/in_process_simulator.h"

namespace eos::runtime::northbound {
namespace {

constexpr const char* kRuntimeId = "018f22d2-82ab-7def-9234-56789abcdef0";

TEST(GatewayCoreStandaloneTest, BlockingHardwareTickDoesNotBlockMonitorReads) {
  std::promise<void> tick_entered;
  auto tick_entered_future = tick_entered.get_future();
  std::promise<void> release_tick;
  auto release_tick_future = release_tick.get_future().share();
  NorthboundCore core({}, [&] {
    tick_entered.set_value();
    release_tick_future.wait();
  });

  auto ticking = std::async(std::launch::async, [&] { core.Tick(); });
  ASSERT_EQ(tick_entered_future.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto monitoring =
      std::async(std::launch::async, [&] { return core.ReadMonitor("cam-0", ""); });
  const bool monitor_completed =
      monitoring.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;

  release_tick.set_value();
  ticking.get();
  monitoring.wait();
  EXPECT_TRUE(monitor_completed);
}

class TemporaryStoreDirectory {
 public:
  TemporaryStoreDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-northbound-navigation-" + lib::MintUuidV7())) {}
  ~TemporaryStoreDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

class CorrelationSink final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  std::string correlation_id;
  std::string message;

 protected:
  void sink_it_(const spdlog::details::log_msg& entry) override {
    correlation_id = lib::CurrentCorrelationId();
    message.assign(entry.payload.data(), entry.payload.size());
  }

  void flush_() override {}
};

class StaticModelSession final : public models::StreamSession {
 public:
  StaticModelSession(nlohmann::json response, std::shared_ptr<nlohmann::json> request)
      : response_(std::move(response)), request_(std::move(request)) {}

  models::CallResult Call(const nlohmann::json& request, const models::CancelRequested&) override {
    *request_ = request;
    return {models::CallResult::Status::kCompleted, 0, response_, {}};
  }
  void Close() noexcept override {}

 private:
  nlohmann::json response_;
  std::shared_ptr<nlohmann::json> request_;
};

class StaticModelAdapter final : public models::Adapter {
 public:
  StaticModelAdapter(nlohmann::json response, std::shared_ptr<nlohmann::json> request)
      : response_(std::move(response)), request_(std::move(request)) {}

  nlohmann::json PrepareRequest(const models::ModelConfig&,
                                const nlohmann::json& request) const override {
    return request;
  }
  nlohmann::json DecodeResponse(const models::ModelConfig&, std::string_view,
                                std::string_view) const override {
    return response_;
  }
  void NormalizeResponse(const models::ModelConfig&, nlohmann::json*) const override {}
  std::unique_ptr<models::StreamSession> CreateStreamSession(
      const models::ModelConfig&) const override {
    return std::make_unique<StaticModelSession>(response_, request_);
  }

 private:
  nlohmann::json response_;
  std::shared_ptr<nlohmann::json> request_;
};

class GatewayCoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    registry_ = std::make_unique<hardware_runtime::Registry>(hardware_runtime::Registry::Adopt(
        hardware_runtime::LoadBinding(std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo"),
        kRuntimeId, simulator_.socket_path()));
    contracts_ = std::make_unique<init::SchemaStore>(EOS_CONTRACTS_ROOT);
    projection_ = std::make_unique<perception::body::Reader>(*registry_, clock_);
    dispatcher_ = std::make_unique<dispatch::Dispatcher>(*registry_, *contracts_);
    models::ModelConfig model;
    model.id = "test-vad";
    model.capability = models::Capability::kVad;
    model.endpoint = "http://127.0.0.1:1/v1/audio/vad";
    model.request_contract = "perception/model/vad_request.v1.json";
    model.response_contract = "perception/model/vad_response.v1.json";
    models::ModelConfig disabled = model;
    disabled.id = "disabled-vad";
    disabled.enabled = false;
    models_ = std::make_unique<models::Runtime>(std::vector<models::ModelConfig>{model, disabled});
    core_ = std::make_unique<NorthboundCore>(
        NorthboundServices{projection_.get(), dispatcher_.get(), registry_.get(), models_.get()},
        [this] { registry_->Tick(0.1); });
    core_->Tick();
  }

  test::InProcessSimulator simulator_;
  lib::ManualClock clock_;
  std::unique_ptr<hardware_runtime::Registry> registry_;
  std::unique_ptr<init::SchemaStore> contracts_;
  std::unique_ptr<perception::body::Reader> projection_;
  std::unique_ptr<dispatch::Dispatcher> dispatcher_;
  std::unique_ptr<models::Runtime> models_;
  std::unique_ptr<NorthboundCore> core_;
};

TEST(GatewayCoreStandaloneTest, StatusWorksWithoutComposedServices) {
  NorthboundCore core({}, nullptr);
  const RpcResult status = core.GetStatus("test-correlation");
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.payload["status"], "degraded");
  EXPECT_EQ(status.payload["version"], "1.0.0");
  EXPECT_EQ(status.payload["correlation_id"], "test-correlation");
  EXPECT_TRUE(status.payload["robots"].empty());
  ASSERT_EQ(status.payload["subsystems"].size(), 1u);
  EXPECT_FALSE(status.payload["subsystems"][0].contains("restarts"));
  EXPECT_FALSE(status.payload["subsystems"][0].contains("last_error"));
  EXPECT_EQ(core.Discover().code, grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(core.ListResources().code, grpc::StatusCode::UNAVAILABLE);
  EXPECT_EQ(core.CaptureFrames("robot", "camera-0", 1).code, grpc::StatusCode::NOT_FOUND);
}

TEST(GatewayCoreStandaloneTest, NavigationPointCrudUsesTypedStoreErrors) {
  TemporaryStoreDirectory directory;
  store::NavigationPointStore navigation_points(directory.path);
  NorthboundServices services;
  services.navigation_points = &navigation_points;
  NorthboundCore core(services, nullptr);

  ASSERT_TRUE(core.PutNavigationPoint("demo-0", "home", 1.0, 2.0, 0.5).ok());
  const RpcResult listed = core.ListNavigationPoints("demo-0");
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.payload.at("points").size(), 1u);
  EXPECT_DOUBLE_EQ(core.GetNavigationPoint("demo-0", "home").payload.at("yaw_rad"), 0.5);
  EXPECT_EQ(core.GetNavigationPoint("demo-0", "missing").code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core.PutNavigationPoint("demo-0", "bad/name", 1.0, 2.0, 3.0).code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(core.DeleteNavigationPoint("demo-0", "home").ok());
  EXPECT_EQ(core.ReloadNavigationPoints().payload.at("point_count"), 0u);
}

TEST(GatewayGrpcServiceTest, StatusUsesTheRequestCorrelationScope) {
  NorthboundCore core({}, nullptr);
  NorthboundGrpcService service(core);
  grpc::ServerContext context;
  google::protobuf::Empty request;
  google::protobuf::Struct response;
  auto sink = std::make_shared<CorrelationSink>();
  auto logger = lib::GetLogger("northbound");
  logger->sinks().push_back(sink);

  ASSERT_TRUE(service.GetStatus(&context, &request, &response).ok());
  const std::string correlation_id = response.fields().at("correlation_id").string_value();
  logger->sinks().erase(std::find(logger->sinks().begin(), logger->sinks().end(), sink));
  EXPECT_EQ(correlation_id.size(), 36u);
  EXPECT_EQ(sink->correlation_id, correlation_id);
  EXPECT_NE(sink->message.find("[grpc_request_completed]"), std::string::npos);
  EXPECT_NE(sink->message.find("method=GetStatus"), std::string::npos);
  EXPECT_NE(sink->message.find("status=0"), std::string::npos);
  EXPECT_NE(sink->message.find("duration_ms="), std::string::npos);
  EXPECT_TRUE(lib::CurrentCorrelationId().empty());
}

TEST(GatewayGrpcServiceTest, FailureStatusNamesRemainActionable) {
  EXPECT_STREQ(GrpcStatusName(grpc::StatusCode::UNAVAILABLE), "UNAVAILABLE");
  EXPECT_STREQ(GrpcReasonCategory(grpc::StatusCode::UNAVAILABLE), "dependency_unavailable");
  EXPECT_STREQ(GrpcStatusName(grpc::StatusCode::INVALID_ARGUMENT), "INVALID_ARGUMENT");
  EXPECT_STREQ(GrpcReasonCategory(grpc::StatusCode::INVALID_ARGUMENT), "invalid_request");
}

TEST(GatewayGrpcServiceTest, ConvertsTypedPerceptionRequestsAndResponses) {
  constexpr std::uint64_t kTrackId = (std::uint64_t{1} << 40) + 7;
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  for (int i = 0; i < 17; ++i) {
    points.push_back({i, i + 1});
    scores.push_back(0.9);
  }
  const std::map<models::Capability, nlohmann::json> responses{
      {models::Capability::kVad,
       {{"speech", true},
        {"probability", 0.8},
        {"segments", {{{"start_s", 0.1}, {"end_s", 0.4}, {"confidence", 0.8}}}}}},
      {models::Capability::kHuman,
       {{"persons", {{{"track_id", kTrackId}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.9}}}},
        {"person_count", 1},
        {"furniture", {{{"bbox", {3, 4, 20, 25}}, {"class_name", "chair"}}}}}},
      {models::Capability::kHumanPose,
       {{"persons",
         {{{"track_id", kTrackId},
           {"bbox", {1, 2, 30, 40}},
           {"confidence", 0.9},
           {"distance_m", nullptr},
           {"keypoints", points},
           {"keypoint_scores", scores},
           {"posture_state", "standing"},
           {"posture", "standing"},
           {"posture_confidence", 0.9},
           {"behavior_event", "none"},
           {"behavior_confidence", 0.8},
           {"pose", "stand"},
           {"pose_confidence", 0.9},
           {"action", "none"},
           {"action_confidence", 0.8}}}},
        {"person_count", 1}}},
      {models::Capability::kFace,
       {{"status", "success"},
        {"operation", "recognize"},
        {"data",
         {{"faces",
           {{{"identity", "Ada"},
             {"known", true},
             {"similarity", 0.98},
             {"bbox", {2, 3, 20, 30}},
             {"direction_px", 4.0}}}},
          {"face_count", 1u}}}}}};
  const std::map<models::Capability, std::string> ids{{models::Capability::kVad, "vad"},
                                                      {models::Capability::kHuman, "human"},
                                                      {models::Capability::kHumanPose, "pose"},
                                                      {models::Capability::kFace, "face"}};
  auto captured = std::make_shared<nlohmann::json>();
  std::vector<models::ModelConfig> configs;
  for (const auto& [capability, id] : ids) {
    models::ModelConfig config;
    config.id = id;
    config.capability = capability;
    config.endpoint = "ws://127.0.0.1:1/model";
    config.transport = models::Transport::kWebSocket;
    configs.push_back(std::move(config));
  }
  models::Runtime runtime(
      std::move(configs), [](const std::string&, const nlohmann::json&) {},
      [&](const models::ModelConfig& config) {
        return std::make_shared<StaticModelAdapter>(responses.at(config.capability), captured);
      });
  NorthboundCore core({nullptr, nullptr, nullptr, &runtime}, nullptr);
  NorthboundGrpcService service(core);

  grpc::ServerContext vad_context;
  island::eos::v1::InferVadRequest vad_request;
  vad_request.set_model_id("vad");
  vad_request.mutable_input()->set_data("RIFF");
  vad_request.set_threshold(0.6);
  island::eos::v1::InferVadResponse vad_response;
  const grpc::Status vad_status = service.InferVad(&vad_context, &vad_request, &vad_response);
  ASSERT_TRUE(vad_status.ok()) << vad_status.error_message();
  EXPECT_EQ(captured->at("input").at("data_b64"), "UklGRg==");
  EXPECT_EQ(captured->at("threshold"), 0.6);
  ASSERT_EQ(vad_response.segments_size(), 1);

  grpc::ServerContext human_context;
  island::eos::v1::DetectHumansRequest human_request;
  human_request.set_model_id("human");
  human_request.mutable_input()->set_data("image");
  island::eos::v1::DetectHumansResponse human_response;
  ASSERT_TRUE(service.DetectHumans(&human_context, &human_request, &human_response).ok());
  ASSERT_EQ(human_response.persons_size(), 1);
  EXPECT_EQ(human_response.persons(0).track_id(), kTrackId);
  EXPECT_EQ(human_response.persons(0).bbox().x2(), 30);
  ASSERT_EQ(human_response.furniture_size(), 1);
  EXPECT_EQ(human_response.furniture(0).class_name(), "chair");

  grpc::ServerContext pose_context;
  island::eos::v1::EstimateHumanPoseRequest pose_request;
  pose_request.set_model_id("pose");
  pose_request.mutable_input()->set_data("image");
  auto* tracked = pose_request.add_persons();
  tracked->set_track_id(kTrackId);
  tracked->mutable_bbox()->set_x2(30);
  tracked->set_confidence(0.9);
  *pose_request.add_furniture() = human_response.furniture(0);
  island::eos::v1::EstimateHumanPoseResponse pose_response;
  ASSERT_TRUE(service.EstimateHumanPose(&pose_context, &pose_request, &pose_response).ok());
  EXPECT_EQ(captured->at("persons").at(0).at("track_id"), kTrackId);
  EXPECT_EQ(captured->at("furniture").at(0).at("class_name"), "chair");
  ASSERT_EQ(pose_response.persons_size(), 1);
  EXPECT_EQ(pose_response.persons(0).keypoints_size(), 17);
  EXPECT_EQ(pose_response.persons(0).track_id(), kTrackId);
  EXPECT_FALSE(pose_response.persons(0).has_distance_m());
  EXPECT_EQ(pose_response.persons(0).pose(), "stand");
  EXPECT_DOUBLE_EQ(pose_response.persons(0).pose_confidence(), 0.9);
  EXPECT_EQ(pose_response.persons(0).action(), "none");
  EXPECT_DOUBLE_EQ(pose_response.persons(0).action_confidence(), 0.8);

  grpc::ServerContext face_context;
  island::eos::v1::DetectFacesRequest face_request;
  face_request.set_model_id("face");
  face_request.mutable_input()->set_data("image");
  island::eos::v1::DetectFacesResponse face_response;
  const grpc::Status face_status =
      service.DetectFaces(&face_context, &face_request, &face_response);
  ASSERT_TRUE(face_status.ok()) << face_status.error_message();
  EXPECT_EQ(captured->at("operation"), "recognize");
  ASSERT_EQ(face_response.data().faces_size(), 1);
  EXPECT_EQ(face_response.data().faces(0).identity(), "Ada");
  EXPECT_TRUE(face_response.data().faces(0).has_direction_px());
}

TEST(GatewayGrpcServiceTest, MapsResponseJsonToProtobufFailureToDataLoss) {
  models::ModelConfig config;
  config.id = "human";
  config.capability = models::Capability::kHuman;
  config.endpoint = "ws://127.0.0.1:1/model";
  config.transport = models::Transport::kWebSocket;
  auto captured = std::make_shared<nlohmann::json>();
  models::Runtime runtime(
      {config}, [](const std::string&, const nlohmann::json&) {},
      [&](const models::ModelConfig&) {
        return std::make_shared<StaticModelAdapter>(
            nlohmann::json{
                {"persons", {{{"track_id", -1}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.9}}}},
                {"person_count", 1}},
            captured);
      });
  NorthboundCore core({nullptr, nullptr, nullptr, &runtime}, nullptr);
  NorthboundGrpcService service(core);
  grpc::ServerContext context;
  island::eos::v1::DetectHumansRequest request;
  request.set_model_id("human");
  request.mutable_input()->set_data("image");
  island::eos::v1::DetectHumansResponse response;

  EXPECT_EQ(service.DetectHumans(&context, &request, &response).error_code(),
            grpc::StatusCode::DATA_LOSS);
}

TEST(GatewayResourceHistoryTest, CatalogReadsQueriesAndWaitsOnPublishedHistory) {
  using namespace std::chrono_literals;
  const std::string uri = "robots/demo-0/detection/state";
  perception::ProductHistory history({uri, "island://schemas/detection.v1.json", "boot-1",
                                      "epoch-1", [](const nlohmann::json&) {}},
                                     2);
  auto writer = history.TakeWriter();
  writer.Append(std::chrono::system_clock::time_point{} + 1s, {{"label", "one"}});
  writer.Append(std::chrono::system_clock::time_point{} + 2s, {{"label", "two"}});
  writer.Append(std::chrono::system_clock::time_point{} + 3s, {{"label", "three"}});
  perception::ResourceCatalog catalog;
  catalog.Register(history.Reader());
  NorthboundServices services;
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);

  const RpcResult listed = core.ListResources();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.payload.size(), 1u);
  EXPECT_EQ(listed.payload[0].at("uri"), uri);
  EXPECT_TRUE(listed.payload[0].at("history"));
  EXPECT_EQ(core.ReadResource(uri).payload.at("label"), "three");

  ResourceHistoryQuery exact;
  exact.kind = ResourceHistoryQuery::Kind::kSequence;
  exact.sequence = 1;
  const RpcResult expired = core.QueryResourceHistory(uri, exact);
  ASSERT_TRUE(expired.ok()) << expired.message;
  EXPECT_TRUE(expired.payload.at("products").empty());
  EXPECT_TRUE(expired.payload.at("retention_gap"));
  EXPECT_EQ(expired.payload.at("retention").at("first_available_sequence"), 2u);

  ResourceHistoryQuery range;
  range.kind = ResourceHistoryQuery::Kind::kSequenceRange;
  range.sequence = 1;
  range.last_sequence = 3;
  range.limit = 1;
  const RpcResult ranged = core.QueryResourceHistory(uri, range);
  ASSERT_TRUE(ranged.ok()) << ranged.message;
  ASSERT_EQ(ranged.payload.at("products").size(), 1u);
  EXPECT_EQ(ranged.payload.at("products")[0].at("sequence"), 2u);
  EXPECT_TRUE(ranged.payload.at("retention_gap"));
  EXPECT_TRUE(ranged.payload.at("truncated"));

  range.sequence = 0;
  EXPECT_EQ(core.QueryResourceHistory(uri, range).code, grpc::StatusCode::INVALID_ARGUMENT);

  auto waiting = std::async(std::launch::async, [&core, &uri] {
    return core.WaitForPublishedResource(uri, 3, [] { return false; });
  });
  writer.Append(std::chrono::system_clock::time_point{} + 4s, {{"label", "four"}});
  const auto published = waiting.get();
  ASSERT_TRUE(published);
  ASSERT_TRUE(published->product);
  EXPECT_EQ(published->product->sequence, 4u);
}

TEST_F(GatewayCoreTest, CameraObjectResourceIsDemandDrivenValidatedAndRetained) {
  using namespace std::chrono_literals;
  const std::string uri = "robots/demo-0/detection/head/state";
  const std::string schema = "egia.contracts.perception.world.detection.v1";
  perception::ProductHistory history(
      {uri, schema, "boot-head", "epoch-head",
       [this, &schema](const nlohmann::json& value) { contracts_->ValidateById(schema, value); }},
      4);
  auto writer = history.TakeWriter();
  const auto snapshot = [](std::uint64_t source_sequence, std::string label) {
    return nlohmann::json{{"type", "detection.snapshot"},
                          {"schema_version", "1.0"},
                          {"sequence", source_sequence},
                          {"timestamp", "2026-10-04T00:00:00Z"},
                          {"source", "cam-0"},
                          {"source_sequence", source_sequence},
                          {"source_generation", source_sequence},
                          {"detections",
                           {{{"id", "obj:bottle-0"},
                             {"kind", "object"},
                             {"label", std::move(label)},
                             {"confidence", 0.9}}}}};
  };

  int starts = 0;
  int stops = 0;
  perception::ProducerCoordinator producers(
      {{"camera_object", {}, [&] {
          if (++starts == 2) {
            writer.Append(std::chrono::system_clock::time_point{} + 3s, snapshot(3, "cold-read"));
          }
          return [&] { ++stops; };
        }}});
  perception::ResourceCatalog catalog;
  catalog.Register(history.Reader(), [&] { return producers.Acquire("camera_object"); }, 100ms);
  NorthboundServices services;
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);

  const RpcResult listed = core.ListResources();
  ASSERT_TRUE(listed.ok()) << listed.message;
  ASSERT_EQ(listed.payload.size(), 1u);
  EXPECT_EQ(listed.payload.at(0).at("uri"), uri);
  EXPECT_EQ(starts, 0);
  EXPECT_FALSE(core.LatestPublishedResource(uri)->product);

  auto watch = core.AcquireResourceDemand(uri);
  auto skill_subscription = core.AcquireResourceDemand(uri);
  ASSERT_TRUE(watch);
  ASSERT_TRUE(skill_subscription);
  EXPECT_EQ(starts, 1);
  EXPECT_EQ(producers.Status("camera_object").consumer_count, 2u);

  EXPECT_THROW(writer.Append(std::chrono::system_clock::time_point{} + 500ms,
                             {{"type", "detection.snapshot"}}),
               std::exception);
  nlohmann::json first = snapshot(1, "bottle");
  writer.Append(std::chrono::system_clock::time_point{} + 1s, first);
  first["detections"][0]["label"] = "mutated";
  const auto watched = core.WaitForPublishedResource(uri, 0, [] { return false; });
  ASSERT_TRUE(watched);
  ASSERT_TRUE(watched->product);
  EXPECT_EQ(watched->product->value.at("source_sequence"), 1u);
  writer.Append(std::chrono::system_clock::time_point{} + 2s, snapshot(2, "paper_ball"));

  ResourceHistoryQuery range;
  range.kind = ResourceHistoryQuery::Kind::kSequenceRange;
  range.sequence = 1;
  range.last_sequence = 2;
  range.limit = 4;
  const RpcResult retained = core.QueryResourceHistory(uri, range);
  ASSERT_TRUE(retained.ok()) << retained.message;
  ASSERT_EQ(retained.payload.at("products").size(), 2u);
  EXPECT_EQ(retained.payload.at("products").at(0).at("value").at("detections").at(0).at("label"),
            "bottle");
  EXPECT_EQ(retained.payload.at("products").at(1).at("value").at("source_sequence"), 2u);

  watch.reset();
  EXPECT_EQ(stops, 0);
  skill_subscription.reset();
  EXPECT_EQ(stops, 1);

  const RpcResult cold_read = core.ReadResource(uri);
  ASSERT_TRUE(cold_read.ok()) << cold_read.message;
  EXPECT_EQ(cold_read.payload.at("source_sequence"), 3u);
  EXPECT_EQ(starts, 2);
  EXPECT_EQ(stops, 2);
}

TEST_F(GatewayCoreTest, VadRetainedResourceSupportsListReadWatchAndHistory) {
  using namespace std::chrono_literals;
  const std::string uri = "robots/demo-0/world/vad/state";
  perception::ProductHistory history(
      {uri, "egia.contracts.perception.world.vad.v1", "boot-vad", "epoch-vad",
       [this](const nlohmann::json& value) {
         contracts_->ValidateById("egia.contracts.perception.world.vad.v1", value);
       }},
      4);
  auto writer = history.TakeWriter();
  perception::ResourceCatalog catalog;
  catalog.Register(history.Reader());
  NorthboundServices services;
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);

  const RpcResult listed = core.ListResources();
  ASSERT_TRUE(listed.ok()) << listed.message;
  ASSERT_EQ(listed.payload.size(), 1u);
  EXPECT_EQ(listed.payload.at(0).at("uri"), uri);
  EXPECT_TRUE(listed.payload.at(0).at("history"));

  writer.Append(std::chrono::system_clock::time_point{} + 1s,
                {{"type", "vad.snapshot"},
                 {"schema_version", "1.0"},
                 {"sequence", 1},
                 {"timestamp", "2026-10-04T00:00:00Z"},
                 {"source_sequence", 41},
                 {"source_timestamp_ns", 9001},
                 {"speech", true},
                 {"probability", 0.92}});
  const RpcResult read = core.ReadResource(uri);
  ASSERT_TRUE(read.ok()) << read.message;
  EXPECT_EQ(read.payload.at("source_sequence"), 41u);
  EXPECT_FALSE(read.payload.contains("pcm"));
  EXPECT_FALSE(read.payload.contains("wav"));
  EXPECT_FALSE(read.payload.contains("utterance"));

  const auto latest = core.LatestPublishedResource(uri);
  ASSERT_TRUE(latest);
  ASSERT_TRUE(latest->product);
  EXPECT_EQ(latest->product->sequence, 1u);

  ResourceHistoryQuery query;
  query.kind = ResourceHistoryQuery::Kind::kSequence;
  query.sequence = 1;
  const RpcResult retained = core.QueryResourceHistory(uri, query);
  ASSERT_TRUE(retained.ok()) << retained.message;
  ASSERT_EQ(retained.payload.at("products").size(), 1u);
  EXPECT_EQ(retained.payload.at("products").at(0).at("value").at("source_timestamp_ns"), 9001);

  auto watching = std::async(std::launch::async, [&] {
    return core.WaitForPublishedResource(uri, 1, [] { return false; });
  });
  writer.Append(std::chrono::system_clock::time_point{} + 2s,
                {{"type", "vad.snapshot"},
                 {"schema_version", "1.0"},
                 {"sequence", 2},
                 {"timestamp", "2026-10-04T00:00:01Z"},
                 {"source_sequence", 42},
                 {"speech", false},
                 {"probability", 0.1}});
  const auto changed = watching.get();
  ASSERT_TRUE(changed);
  ASSERT_TRUE(changed->product);
  EXPECT_EQ(changed->product->value.at("source_sequence"), 42u);
}

TEST(GatewayResourceHistoryTest, GrpcMapsTypedTimestampAndRangeQueries) {
  using namespace std::chrono_literals;
  perception::ProductHistory history({"resource/state", "island://schemas/resource.v1.json",
                                      "boot-1", "epoch-1", [](const nlohmann::json&) {}},
                                     4);
  auto writer = history.TakeWriter();
  writer.Append(std::chrono::system_clock::time_point{} + 1s, {{"value", 1}});
  writer.Append(std::chrono::system_clock::time_point{} + 3s, {{"value", 3}});
  perception::ResourceCatalog catalog;
  catalog.Register(history.Reader());
  NorthboundServices services;
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);
  NorthboundGrpcService service(core);

  grpc::ServerContext context;
  island::eos::v1::QueryResourceHistoryRequest request;
  request.set_uri("resource/state");
  request.mutable_nearest()->set_seconds(2);
  island::eos::v1::QueryResourceHistoryResponse response;
  const grpc::Status status = service.QueryResourceHistory(&context, &request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  ASSERT_EQ(response.products_size(), 1);
  EXPECT_EQ(response.products(0).sequence(), 1u);
  EXPECT_EQ(response.products(0).value().fields().at("value").number_value(), 1);

  grpc::ServerContext invalid_context;
  island::eos::v1::QueryResourceHistoryRequest invalid;
  invalid.set_uri("resource/state");
  invalid.mutable_sequence_range()->set_first_sequence(1);
  invalid.mutable_sequence_range()->set_last_sequence(3);
  island::eos::v1::QueryResourceHistoryResponse invalid_response;
  EXPECT_EQ(
      service.QueryResourceHistory(&invalid_context, &invalid, &invalid_response).error_code(),
      grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(GatewayResourceHistoryTest, WatchMapsProducerFactoryFailureToUnavailable) {
  perception::ProductHistory history(
      {"resource/state", "schema", "boot", "epoch", [](const nlohmann::json&) {}}, 2);
  history.TakeWriter();
  perception::ResourceCatalog catalog;
  catalog.Register(
      history.Reader(),
      []() -> perception::ProducerLease { throw std::runtime_error("factory failed"); },
      std::chrono::milliseconds(100));
  NorthboundServices services;
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);
  NorthboundGrpcService service(core);
  grpc::ServerContext context;
  island::eos::v1::ResourceRequest request;
  request.set_uri("resource/state");

  const grpc::Status status = service.WatchResource(&context, &request, nullptr);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
  EXPECT_NE(status.error_message().find("factory failed"), std::string::npos);
}

TEST_F(GatewayCoreTest, ModelStateIsNotANorthboundResource) {
  const RpcResult status = core_->GetStatus("models-summary");
  ASSERT_TRUE(status.ok()) << status.message;
  EXPECT_EQ(status.payload.at("devices_total"), 7u);
  EXPECT_EQ(status.payload.at("devices_alive"), 7u);
  EXPECT_EQ(status.payload.at("devices_admitted"), 7u);
  EXPECT_EQ(status.payload.at("models"),
            nlohmann::json({{"status", "degraded"},
                            {"total", 1},
                            {"ready", 0},
                            {"checking", 0},
                            {"degraded", 0},
                            {"unavailable", 1},
                            {"items",
                             {{{"id", "disabled-vad"},
                               {"capability", "audio.vad"},
                               {"health", "unavailable"},
                               {"message", "model disabled"}},
                              {{"id", "test-vad"},
                               {"capability", "audio.vad"},
                               {"health", "unavailable"},
                               {"message", "health endpoint is not configured"}}}}}));
  const RpcResult resources = core_->ListResources();
  ASSERT_TRUE(resources.ok()) << resources.message;
  EXPECT_EQ(std::find_if(resources.payload.begin(), resources.payload.end(),
                         [](const auto& item) {
                           return item.contains("class") && item.at("class").is_string() &&
                                  item.at("class") == "model";
                         }),
            resources.payload.end());
  EXPECT_EQ(core_->ReadResource("robots/demo-0/models/test-vad/state").code,
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GatewayCoreTest, TypedModelStatusIncludesDisabledAndOmitsEndpoint) {
  const RpcResult enabled = core_->GetModelStatus("test-vad");
  ASSERT_TRUE(enabled.ok()) << enabled.message;
  EXPECT_EQ(enabled.payload.at("capability"), "audio.vad");
  EXPECT_TRUE(enabled.payload.at("enabled").get<bool>());
  EXPECT_FALSE(enabled.payload.contains("endpoint"));
  EXPECT_FALSE(enabled.payload.contains("provider_model_id"));

  const RpcResult disabled = core_->GetModelStatus("disabled-vad");
  ASSERT_TRUE(disabled.ok()) << disabled.message;
  EXPECT_FALSE(disabled.payload.at("enabled").get<bool>());
  EXPECT_EQ(disabled.payload.at("status"), "unavailable");
  EXPECT_EQ(core_->GetModelStatus("missing").code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->GetModelStatus("").code, grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GatewayCoreTest, CapturesOnlyBoundedCameraFrames) {
  EXPECT_EQ(core_->CaptureFrames("demo-0", "cam-0", 0).code, grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->CaptureFrames("demo-0", "base-0", 1).code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->CaptureFrames("demo-0", "cam-0", 2).code, grpc::StatusCode::UNAVAILABLE);
}

TEST_F(GatewayCoreTest, ObjectObservationValidatesBoundCameraAndDetectionModel) {
  EXPECT_EQ(core_->ObserveObjects("demo-0", "cam-0", "test-vad", 0).code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->ObserveObjects("", "cam-0", "object-model").code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->ObserveObjects("other-robot", "cam-0", "object-model").code,
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ObserveObjects("demo-0", "base-0", "object-model").code,
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ObserveObjects("demo-0", "cam-0", "object-model").code,
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ObserveObjects("demo-0", "cam-0", "test-vad").code,
            grpc::StatusCode::FAILED_PRECONDITION);
}

TEST_F(GatewayCoreTest, TypedInferenceEnforcesExactModelCapability) {
  EXPECT_EQ(core_->InferModel("", models::Capability::kVad, nlohmann::json::object(), {}).code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(
      core_->InferModel("missing", models::Capability::kVad, nlohmann::json::object(), {}).code,
      grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(
      core_->InferModel("disabled-vad", models::Capability::kVad, nlohmann::json::object(), {})
          .code,
      grpc::StatusCode::FAILED_PRECONDITION);
  const RpcResult wrong =
      core_->InferModel("test-vad", models::Capability::kFace, nlohmann::json::object(), {});
  EXPECT_EQ(wrong.code, grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_NE(wrong.message.find("vision.face"), std::string::npos);
}

TEST_F(GatewayCoreTest, MonitorValidatesDeviceKindsAndComputesBoundedAudioEnergy) {
  EXPECT_EQ(core_->ValidateMonitor("", "mic-0").code, grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->ValidateMonitor("base-0", "mic-0").code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ValidateMonitor("cam-0", "base-0").code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ValidateMonitor("cam-0", "mic-0").code, grpc::StatusCode::OK);

  hardware::MicrophoneTelemetry pcm;
  pcm.sample_rate_hz = 16000;
  pcm.channels = 1;
  pcm.pcm = {0, 16384, -16384, 32767};
  const MonitorAudioSample audio = CalculateAudioEnergy(pcm, 7, std::chrono::system_clock::now());
  EXPECT_EQ(audio.sequence, 7u);
  EXPECT_GT(audio.rms, 0.0);
  EXPECT_LE(audio.rms, 1.0);
  EXPECT_GE(audio.peak, audio.rms);
  EXPECT_LE(audio.peak, 1.0);
  EXPECT_GE(audio.dbfs, -96.0);
  EXPECT_LE(audio.dbfs, 0.0);
  EXPECT_TRUE(audio.clipping);
  EXPECT_EQ(audio.sample_rate_hz, 16000u);
  EXPECT_EQ(audio.channels, 1u);
}

TEST_F(GatewayCoreTest, TelemetryHubFansOutDestructiveMicrophoneWindows) {
  hardware_runtime::TelemetryHub hub(*registry_);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!hub.LatestMicrophone("mic-0") && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

  ASSERT_TRUE(hub.LatestMicrophone("mic-0").has_value());
  const auto vad = hub.ConsumeMicrophone("mic-0", "vad-test");
  const auto independent = hub.ConsumeMicrophone("mic-0", "other-test");
  ASSERT_TRUE(vad.has_value());
  ASSERT_TRUE(independent.has_value());
  EXPECT_EQ(independent->pcm, vad->pcm);
}

TEST_F(GatewayCoreTest, DiscoversStableRuntimeHardwareAndDevices) {
  const RpcResult first = core_->Discover();
  const RpcResult second = core_->Discover();
  ASSERT_TRUE(first.ok()) << first.message;
  ASSERT_TRUE(second.ok()) << second.message;
  EXPECT_EQ(first.payload.at("robot").at("id"), kRuntimeId);
  EXPECT_EQ(second.payload.at("robot"), first.payload.at("robot"));
  EXPECT_EQ(first.payload.at("robot").at("name"), "demo-0");
  EXPECT_EQ(first.payload.at("robot").at("model"), "eos-demo");
  EXPECT_EQ(first.payload.at("robot").at("description"),
            "Deterministic seven-device loopback robot for development, examples, and tests.");
  EXPECT_EQ(first.payload.at("robot").at("health"), "ready");
  ASSERT_EQ(first.payload.at("robot").at("primitives").size(), 2u);
  EXPECT_EQ(first.payload.at("robot").at("primitives").at(0).at("name"), "robot.stop");
  EXPECT_EQ(first.payload.at("robot").at("primitives").at(1).at("name"), "safety.estop");
  ASSERT_EQ(first.payload.at("devices").size(), 7u);

  const auto base =
      std::find_if(first.payload.at("devices").begin(), first.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "base-0"; });
  ASSERT_NE(base, first.payload.at("devices").end());
  EXPECT_EQ(base->at("class"), "base");
  EXPECT_EQ(base->at("part"), "default");
  EXPECT_EQ(base->at("description"), "planar drive stub");
  EXPECT_EQ(base->at("state_uri"), "robots/demo-0/devices/base-0/state");
  EXPECT_EQ(base->at("state"), "idle");
  EXPECT_EQ(base->at("health"), "ready");
  const auto camera_state =
      std::find_if(first.payload.at("devices").begin(), first.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "cam-0"; });
  ASSERT_NE(camera_state, first.payload.at("devices").end());
  EXPECT_EQ(camera_state->at("state"), "unknown");
  const auto microphone =
      std::find_if(first.payload.at("devices").begin(), first.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "mic-0"; });
  ASSERT_NE(microphone, first.payload.at("devices").end());
  EXPECT_EQ(microphone->at("state"), "listening");
  const auto navigate =
      std::find_if(base->at("primitives").begin(), base->at("primitives").end(),
                   [](const auto& primitive) { return primitive.at("name") == "base.navigate"; });
  ASSERT_NE(navigate, base->at("primitives").end());
  EXPECT_EQ(navigate->at("kind"), "direct");
  EXPECT_EQ(navigate->at("references"), nlohmann::json::array({"relative", "absolute"}));
  EXPECT_TRUE(navigate->at("limits").contains("x_m"));
  const auto& navigate_parameters = navigate->at("parameters");
  EXPECT_EQ(navigate_parameters.at("type"), "object");
  EXPECT_FALSE(navigate_parameters.at("properties").contains("type"));
  EXPECT_FALSE(navigate_parameters.at("properties").contains("command_id"));
  EXPECT_EQ(navigate_parameters.at("properties").at("reference").at("enum"),
            navigate->at("references"));
  EXPECT_EQ(navigate_parameters.at("properties").at("frame_id").at("enum"),
            nlohmann::json::array({"map"}));
  EXPECT_TRUE(navigate_parameters.contains("allOf"));
  const auto stop =
      std::find_if(base->at("primitives").begin(), base->at("primitives").end(),
                   [](const auto& primitive) { return primitive.at("name") == "motion.stop"; });
  ASSERT_NE(stop, base->at("primitives").end());
  EXPECT_EQ(stop->at("health"), "ready");
  EXPECT_TRUE(stop->at("parameters").at("properties").empty());

  const auto camera =
      std::find_if(first.payload.at("devices").begin(), first.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "cam-0"; });
  ASSERT_NE(camera, first.payload.at("devices").end());
  const auto registration =
      std::find_if(camera->at("primitives").begin(), camera->at("primitives").end(),
                   [](const auto& primitive) { return primitive.at("name") == "face.register"; });
  ASSERT_NE(registration, camera->at("primitives").end());
  EXPECT_EQ(registration->at("kind"), "advanced");
  EXPECT_EQ(registration->at("health"), "unavailable");
  EXPECT_EQ(registration->at("dependencies"),
            nlohmann::json::array({{{"name", "face"}, {"health", "unavailable"}},
                                   {{"name", "camera"}, {"health", "ready"}}}));
}

TEST_F(GatewayCoreTest, DeviceFailureDoesNotFabricateEstop) {
  const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds()).Halt();
  core_->Tick();
  const RpcResult discovery = core_->Discover();
  ASSERT_TRUE(discovery.ok()) << discovery.message;
  EXPECT_FALSE(discovery.payload.at("robot").at("estop"));
  EXPECT_EQ(discovery.payload.at("robot").at("state"), "fault");
  const RpcResult status = core_->GetStatus("halted-device");
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.payload.at("status"), "degraded");
  EXPECT_FALSE(status.payload.at("robots").at(0).at("estop"));
}

TEST_F(GatewayCoreTest, PublicInventoryReadsUseRuntimeSnapshots) {
  const RpcResult discovery = core_->Discover();
  const RpcResult status = core_->GetStatus("before-halt");
  const RpcResult listed = core_->ListResources();
  ASSERT_TRUE(discovery.ok());
  ASSERT_TRUE(status.ok());
  ASSERT_TRUE(listed.ok());

  const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds()).Halt();
  EXPECT_EQ(core_->Discover().payload, discovery.payload);
  EXPECT_EQ(core_->GetStatus("after-halt").payload.at("devices_alive"),
            status.payload.at("devices_alive"));
  EXPECT_EQ(core_->ListResources().payload, listed.payload);

  core_->Tick();
  EXPECT_LT(core_->GetStatus("after-tick").payload.at("devices_alive").get<int>(),
            status.payload.at("devices_alive").get<int>());
}

TEST_F(GatewayCoreTest, DiscoveryReportsTelemetryProvenActivity) {
  hardware::Backend& base_backend =
      const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds());
  ASSERT_EQ(base_backend.ExecuteBase(hardware::BaseCommand{
                0.2, 0.0, 0.0, std::chrono::steady_clock::now(), "active-base"}),
            hardware::ReleaseResult::kAccepted);
  core_->Tick();
  const RpcResult discovery = core_->Discover();
  ASSERT_TRUE(discovery.ok()) << discovery.message;
  const auto base =
      std::find_if(discovery.payload.at("devices").begin(), discovery.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "base-0"; });
  ASSERT_NE(base, discovery.payload.at("devices").end());
  EXPECT_EQ(base->at("state"), "active");
}

TEST_F(GatewayCoreTest, ExplicitEstopIsReportedAsEstop) {
  ASSERT_TRUE(
      core_->CallPrimitive("safety.estop", "1.0.0", "", nlohmann::json::object(), "discover-estop")
          .ok());
  core_->Tick();
  const RpcResult discovery = core_->Discover();
  ASSERT_TRUE(discovery.ok()) << discovery.message;
  EXPECT_TRUE(discovery.payload.at("robot").at("estop"));
  EXPECT_EQ(discovery.payload.at("robot").at("state"), "fault");
  const RpcResult status = core_->GetStatus("explicit-estop");
  ASSERT_TRUE(status.ok());
  EXPECT_TRUE(status.payload.at("robots").at(0).at("estop"));
}

TEST_F(GatewayCoreTest, DiscoveryReportsDegradedDeviceHealth) {
  const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds()).Halt();
  core_->Tick();
  const RpcResult result = core_->Discover();
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_EQ(result.payload.at("robot").at("health"), "degraded");
  const auto base =
      std::find_if(result.payload.at("devices").begin(), result.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "base-0"; });
  ASSERT_NE(base, result.payload.at("devices").end());
  EXPECT_EQ(base->at("health"), "unavailable");
  EXPECT_EQ(base->at("state"), "fault");
}

TEST_F(GatewayCoreTest, DoesNotFabricateWorldResource) {
  const RpcResult result = core_->ReadResource("robots/demo-0/world/state");
  EXPECT_EQ(result.code, grpc::StatusCode::NOT_FOUND);
}

TEST_F(GatewayCoreTest, DetectionIsUnavailableBeforeAModelResult) {
  const RpcResult result = core_->ReadResource("robots/demo-0/detection/state");
  EXPECT_EQ(result.code, grpc::StatusCode::NOT_FOUND);
  const RpcResult resources = core_->ListResources();
  ASSERT_TRUE(resources.ok()) << resources.message;
  EXPECT_EQ(std::find_if(resources.payload.begin(), resources.payload.end(),
                         [](const auto& item) {
                           return item.value("uri", "") == "robots/demo-0/detection/state";
                         }),
            resources.payload.end());
}

TEST_F(GatewayCoreTest, CompactAsrValidatesAgainstV1Contract) {
  models::MicrophoneVadStore store;
  models::MicrophoneVadStore::Snapshot snapshot;
  snapshot.model_id = "vad";
  snapshot.response = {{"speech", true}};
  snapshot.transcript = "Stop.";
  snapshot.asr_model_id = "asr";
  snapshot.asr_response = {{"text", "Stop."}, {"language", "en"}, {"confidence", 0.99}};
  store.Set(std::move(snapshot));
  perception::body::Reader projection(*registry_, clock_, nullptr, nullptr, &store);
  NorthboundCore core(
      NorthboundServices{&projection, dispatcher_.get(), registry_.get(), models_.get()}, nullptr);

  const RpcResult result = core.ReadResource("robots/demo-0/world/asr/state");
  ASSERT_TRUE(result.ok()) << result.message;
  EXPECT_NO_THROW(contracts_->ValidateResource("asr.snapshot", result.payload));
  EXPECT_EQ(result.payload.at("schema_version"), "1.0");
  EXPECT_FALSE(result.payload.contains("segments"));
}

TEST_F(GatewayCoreTest, WatchersShareOneProjectionSamplePerInterval) {
  models::MicrophoneVadStore store;
  models::MicrophoneVadStore::Snapshot snapshot;
  snapshot.model_id = "vad";
  snapshot.response = {{"speech", true}};
  snapshot.transcript = "Stop.";
  snapshot.asr_model_id = "asr";
  snapshot.asr_response = {{"text", "Stop."}, {"language", "en"}, {"confidence", 0.99}};
  store.Set(std::move(snapshot));
  perception::body::Reader projection(*registry_, clock_, nullptr, nullptr, &store);
  NorthboundCore core(
      NorthboundServices{&projection, dispatcher_.get(), registry_.get(), models_.get()}, nullptr);
  const std::string uri = "robots/demo-0/world/asr/state";
  const RpcResult first = core.ReadResourceForWatch(uri);
  const RpcResult shared = core.ReadResourceForWatch(uri);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.payload.at("sequence"), first.payload.at("sequence"));

  std::this_thread::sleep_for(kMinIntervalMs);
  core.Tick();
  const RpcResult refreshed = core.ReadResourceForWatch(uri);
  ASSERT_TRUE(refreshed.ok());
  EXPECT_GT(refreshed.payload.at("sequence"), first.payload.at("sequence"));
}

TEST_F(GatewayCoreTest, UnknownResourceIsNotFound) {
  EXPECT_EQ(core_->ReadResource("warp/state").code, grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_->ReadResource("").code, grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->ReadResource("robots/demo-0/devices/missing/state").code,
            grpc::StatusCode::NOT_FOUND);
}

TEST_F(GatewayCoreTest, BodyReadsUseBoundedRuntimeSnapshots) {
  const std::string uri = "robots/demo-0/devices/base-0/state";
  const RpcResult sampled = core_->ReadResource(uri);
  ASSERT_TRUE(sampled.ok()) << sampled.message;
  ASSERT_TRUE(sampled.sample_age.has_value());
  EXPECT_LT(*sampled.sample_age, std::chrono::seconds(1));
  const_cast<hardware::Backend&>(registry_->Find(hardware::DeviceKind::kBase).feeds()).Halt();
  EXPECT_TRUE(core_->ReadResource(uri).ok());

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  EXPECT_EQ(core_->ReadResource(uri).code, grpc::StatusCode::UNAVAILABLE);
}

TEST_F(GatewayCoreTest, ResourceCatalogPreservesLegacyFallback) {
  perception::ResourceCatalog catalog(projection_.get());
  NorthboundServices services{projection_.get(), dispatcher_.get(), registry_.get(), models_.get()};
  services.resources = &catalog;
  NorthboundCore core(services, nullptr);

  const RpcResult resources = core.ListResources();
  ASSERT_TRUE(resources.ok()) << resources.message;
  EXPECT_EQ(resources.payload, projection_->ListResources());
  EXPECT_TRUE(core.ReadResource("robots/demo-0/devices/base-0/state").ok());
  core.Tick();
  const RpcResult resource = core.ReadResource("robots/demo-0/arm/state");
  ASSERT_TRUE(resource.ok()) << resource.message;
  EXPECT_EQ(resource.payload.at("type"), "arm.snapshot");
  const RpcResult device = core.ReadResource("robots/demo-0/devices/base-0/state");
  ASSERT_TRUE(device.ok()) << device.message;
  EXPECT_EQ(device.payload.at("device_class"), "base");
  EXPECT_FALSE(catalog.FindHistory("robots/demo-0/devices/base-0/state").has_value());
  EXPECT_FALSE(core.LatestPublishedResource("robots/demo-0/devices/base-0/state").has_value());
  EXPECT_TRUE(catalog.SupportsResourceTemplate("robots/{robot_name}/devices/{device_id}/state"));
  EXPECT_TRUE(core.ReadResourceForWatch("robots/demo-0/devices/base-0/state").ok());
  for (const std::string device_class : {"base", "arm", "gripper"}) {
    const std::string uri = "robots/demo-0/" + device_class + "/status";
    const RpcResult status = core.ReadResource(uri);
    ASSERT_TRUE(status.ok()) << status.message;
    EXPECT_EQ(status.payload.at("type"), device_class + ".status");
    EXPECT_TRUE(core.ReadResourceForWatch(uri).ok());
    EXPECT_TRUE(
        catalog.SupportsResourceTemplate("robots/{robot_name}/" + device_class + "/status"));
  }
}

TEST_F(GatewayCoreTest, ListsOnlyReadableResources) {
  const RpcResult resources = core_->ListResources();
  ASSERT_TRUE(resources.ok());
  EXPECT_TRUE(resources.payload.is_array());
  for (const auto& resource : resources.payload) {
    if (resource.at("uri") == "robots") continue;
    EXPECT_NE(resource.at("uri").get<std::string>().find("demo-0"), std::string::npos);
    EXPECT_EQ(resource.at("uri").get<std::string>().find("/vision/state"), std::string::npos);
    EXPECT_EQ(resource.at("uri").get<std::string>().find("/pose/state"), std::string::npos);
    EXPECT_EQ(resource.at("uri").get<std::string>().find("/vad/state"), std::string::npos);
  }
}

TEST_F(GatewayCoreTest, ValidatesAndDispatchesPrimitiveCalls) {
  const RpcResult accepted = core_->CallPrimitive(
      "base.navigate", "1.0.0", "base-0",
      {{"reference", "relative"}, {"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}, "test-1");
  ASSERT_TRUE(accepted.ok()) << accepted.message;
  EXPECT_TRUE(accepted.payload["accepted"]);
  EXPECT_NO_THROW(contracts_->ValidateAck(accepted.payload));

  const RpcResult initialized =
      core_->CallPrimitive("arm.init", "1.0.0", "arm-0", nlohmann::json::object(), "test-init");
  ASSERT_TRUE(initialized.ok()) << initialized.message;
  EXPECT_TRUE(initialized.payload.at("accepted"));
  EXPECT_EQ(initialized.payload.at("result").at("status"), "succeeded");
  EXPECT_NO_THROW(contracts_->ValidateAck(initialized.payload));

  EXPECT_EQ(
      core_->CallPrimitive("arm.wave", "1.0.0", "arm-0", nlohmann::json::object(), "bad-name").code,
      grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(core_
                ->CallPrimitive("base.navigate", "", "base-0", nlohmann::json::object(),
                                "bad-version-empty")
                .code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_
                ->CallPrimitive("base.navigate", "2.0.0", "base-0", nlohmann::json::object(),
                                "bad-version")
                .code,
            grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(
      core_
          ->CallPrimitive("base.navigate", "1.0.0", "base-0",
                          {{"reference", "absolute"}, {"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}},
                          "bad-arguments")
          .code,
      grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_F(GatewayCoreTest, PrimitiveCommandsReplayConflictAndCancelById) {
  const nlohmann::json posture{{"posture", "lie"}};
  const RpcResult first =
      core_->CallPrimitive("base.posture", "1.0.0", "base-0", posture, "core-command-1");
  ASSERT_TRUE(first.ok());
  const RpcResult replay =
      core_->CallPrimitive("base.posture", "1.0.0", "base-0", posture, "core-command-1");
  EXPECT_EQ(replay.payload, first.payload);
  EXPECT_EQ(core_
                ->CallPrimitive("base.posture", "1.0.0", "base-0", {{"posture", "walk"}},
                                "core-command-1")
                .code,
            grpc::StatusCode::ALREADY_EXISTS);
  EXPECT_EQ(core_->CallPrimitive("base.posture", "1.0.0", "base-0", posture, "").code,
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(core_->CancelPrimitive("missing-command").code, grpc::StatusCode::NOT_FOUND);

  auto motion = std::async(std::launch::async, [this] {
    return core_->CallPrimitive("gimbal.motion", "1.0.0", "gimbal-0",
                                {{"reference", "absolute"}, {"pan_rad", 0.4}, {"tilt_rad", 0.2}},
                                "core-gimbal-cancel");
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  const RpcResult cancelled = core_->CancelPrimitive("core-gimbal-cancel");
  ASSERT_TRUE(cancelled.ok()) << cancelled.message;
  EXPECT_EQ(cancelled.payload.at("result").at("status"), "cancelled");
  EXPECT_EQ(motion.get().payload, cancelled.payload);
  EXPECT_EQ(core_->CancelPrimitive("core-gimbal-cancel").payload, cancelled.payload);
}

TEST_F(GatewayCoreTest, OperatorTierListingIsConfigurable) {
  dispatch::Dispatcher operator_dispatcher(*registry_, *contracts_, true);
  NorthboundCore operator_core(
      NorthboundServices{projection_.get(), &operator_dispatcher, registry_.get(), nullptr},
      nullptr);
  const RpcResult discovery = operator_core.Discover();
  ASSERT_TRUE(discovery.ok());
  const auto arm =
      std::find_if(discovery.payload.at("devices").begin(), discovery.payload.at("devices").end(),
                   [](const auto& device) { return device.at("id") == "arm-0"; });
  ASSERT_NE(arm, discovery.payload.at("devices").end());
  const auto movej =
      std::find_if(arm->at("primitives").begin(), arm->at("primitives").end(),
                   [](const auto& primitive) { return primitive.at("name") == "arm.movej"; });
  ASSERT_NE(movej, arm->at("primitives").end());
  const auto& properties = movej->at("parameters").at("properties");
  EXPECT_EQ(properties.at("joint_positions_rad").at("minItems"), 6);
  EXPECT_EQ(properties.at("joint_positions_rad").at("maxItems"), 6);
  EXPECT_FALSE(properties.contains("speed_rad_s"));
}

TEST_F(GatewayCoreTest, StateRejectionIsSuccessfulAcknowledgement) {
  ASSERT_TRUE(
      core_->CallPrimitive("safety.estop", "1.0.0", "", nlohmann::json::object(), "estop-command")
          .ok());
  const RpcResult rejected = core_->CallPrimitive("base.velocity", "1.0.0", "base-0",
                                                  {{"linear_x_m_s", 0.2},
                                                   {"linear_y_m_s", 0.0},
                                                   {"angular_z_rad_s", 0.0},
                                                   {"valid_for_ms", 500}},
                                                  "after-estop");
  ASSERT_TRUE(rejected.ok()) << rejected.message;
  EXPECT_FALSE(rejected.payload.at("accepted"));
  EXPECT_EQ(rejected.payload.at("reason_code"), "rejected.estop");
  EXPECT_NO_THROW(contracts_->ValidateAck(rejected.payload));
}

TEST_F(GatewayCoreTest, TickRunsUnderTheCore) {
  int ticks = 0;
  NorthboundCore ticking({}, [&ticks] { ++ticks; });
  ticking.Tick();
  EXPECT_EQ(ticks, 1);
}

}  // namespace
}  // namespace eos::runtime::northbound

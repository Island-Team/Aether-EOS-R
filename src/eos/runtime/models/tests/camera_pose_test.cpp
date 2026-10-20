#include "src/eos/runtime/models/camera_pose.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <chrono>
#include <thread>
#include <vector>

namespace eos::runtime::models {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

class DelayedSequenceServer {
 public:
  explicit DelayedSequenceServer(std::chrono::milliseconds delay,
                                 std::string body = R"({"persons":[],"person_count":0})")
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
        delay_(delay),
        body_(std::move(body)),
        worker_([this] { Serve(); }) {}
  DelayedSequenceServer(std::chrono::milliseconds delay, std::vector<std::string> bodies)
      : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}),
        delay_(delay),
        bodies_(std::move(bodies)),
        worker_([this] { Serve(); }) {}

  ~DelayedSequenceServer() {
    stopping_ = true;
    asio::io_context wake_io;
    tcp::socket wake_socket(wake_io);
    beast::error_code ignored;
    wake_socket.connect({asio::ip::make_address("127.0.0.1"), port()}, ignored);
    wake_socket.close(ignored);
    if (worker_.joinable()) worker_.join();
    acceptor_.close(ignored);
  }

  std::uint16_t port() const { return acceptor_.local_endpoint().port(); }
  std::size_t requests() const { return requests_.load(); }
  std::string last_request_body() const {
    std::lock_guard<std::mutex> lock(body_mutex_);
    return last_request_body_;
  }
  std::vector<std::string> request_bodies() const {
    std::lock_guard<std::mutex> lock(body_mutex_);
    return request_bodies_;
  }
  void SetBody(std::string body) {
    std::lock_guard<std::mutex> lock(body_mutex_);
    body_ = std::move(body);
  }

 private:
  void Serve() {
    while (!stopping_) {
      tcp::socket socket(io_);
      beast::error_code error;
      acceptor_.accept(socket, error);
      if (error) break;
      if (stopping_) break;
      beast::flat_buffer buffer;
      http::request<http::string_body> request;
      http::read(socket, buffer, request, error);
      if (error) continue;
      {
        std::lock_guard<std::mutex> lock(body_mutex_);
        last_request_body_ = request.body();
        request_bodies_.push_back(request.body());
      }
      const std::size_t request_index = requests_.fetch_add(1);
      std::this_thread::sleep_for(delay_);
      std::string response_body;
      {
        std::lock_guard<std::mutex> lock(body_mutex_);
        response_body = bodies_.empty() ? body_ : bodies_[std::min(request_index, bodies_.size() - 1)];
      }
      http::response<http::string_body> response{http::status::ok, 11};
      response.set(http::field::content_type, "application/json");
      response.body() = std::move(response_body);
      response.prepare_payload();
      http::write(socket, response, error);
      socket.shutdown(tcp::socket::shutdown_both, error);
    }
  }

  asio::io_context io_;
  tcp::acceptor acceptor_;
  std::chrono::milliseconds delay_;
  std::string body_;
  std::vector<std::string> bodies_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> requests_{0};
  mutable std::mutex body_mutex_;
  std::string last_request_body_;
  std::vector<std::string> request_bodies_;
  std::thread worker_;
};

class TestJsonAdapter final : public Adapter {
 public:
  nlohmann::json PrepareRequest(const ModelConfig&, const nlohmann::json& request) const override {
    return request;
  }

  nlohmann::json DecodeResponse(const ModelConfig&, std::string_view,
                                std::string_view body) const override {
    return nlohmann::json::parse(body);
  }

  void NormalizeResponse(const ModelConfig&, nlohmann::json*) const override {}
};

CameraPoseStore::Snapshot Pose(std::uint64_t generation) {
  CameraPoseStore::Snapshot snapshot;
  snapshot.model_id = "posture";
  snapshot.device_id = "camera.front";
  snapshot.generation = generation;
  snapshot.image_width_px = 1280;
  snapshot.image_height_px = 720;
  snapshot.response = {{"persons", nlohmann::json::array()}, {"person_count", 0}};
  return snapshot;
}

TEST(CameraPoseStoreTest, PublishesPoseImmediatelyAndEnrichesLatestWithoutAdvancingSequence) {
  CameraPoseStore store;
  store.SetPose(Pose(1), true);
  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 1);
  EXPECT_EQ(store.Latest()->sequence, 1);
  EXPECT_FALSE(store.Latest()->face_response.has_value());

  store.SetFace(1, {{"data", {{"faces", nlohmann::json::array()}}}});
  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 1);
  EXPECT_EQ(store.Latest()->sequence, 1);
  EXPECT_EQ(store.Latest()->image_width_px, 1280u);
  EXPECT_EQ(store.Latest()->image_height_px, 720u);
  EXPECT_TRUE(store.Latest()->face_response.has_value());
  EXPECT_EQ(store.Latest()->face_generation, 1);

  store.SetPose(Pose(2), true);
  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 2);
  EXPECT_EQ(store.Latest()->sequence, 2);
  EXPECT_TRUE(store.Latest()->face_response.has_value());
  EXPECT_EQ(store.Latest()->face_generation, 1);
}

TEST(CameraPoseStoreTest, GemmaContextDoesNotReplaceYoloPose) {
  CameraPoseStore store;
  auto source = Pose(1);
  store.SetSource(std::move(source), false, false, false, false);
  nlohmann::json humans = {{"persons", {{{"track_id", 1}, {"bbox", {0, 0, 12, 12}},
                                         {"confidence", 0.9}, {"posture_state", "standing"},
                                         {"posture", "standing"}, {"pose", "stand"},
                                         {"action", "fallen"}, {"behavior_event", "fallen_event"}},
                                        {{"track_id", 2}, {"bbox", {0, 0, 6, 6}},
                                         {"confidence", 0.8}, {"posture_state", "sitting"},
                                         {"posture", "sitting"}, {"pose", "sit"}}}},
                           {"person_count", 2}};
  ASSERT_TRUE(store.SetHuman(1, humans));
  EXPECT_EQ(store.Latest()->response["persons"][0]["action"], "unknown");
  ASSERT_TRUE(store.SetGemmaPosture(1, "1", "lying_on_floor", "fallen"));
  auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->response["persons"][0]["posture_state"], "standing");
  EXPECT_FALSE(latest->response["persons"][0].contains("posture"));
  EXPECT_EQ(latest->response["persons"][0]["pose"], "stand");
  EXPECT_EQ(latest->response["persons"][0]["posture_source"], "yolo");
  EXPECT_EQ(latest->response["persons"][0]["action"], "unknown");
  EXPECT_EQ(latest->response["persons"][0]["action_source"], "yolo");
  EXPECT_EQ(latest->response["persons"][1]["posture_state"], "sitting");
  EXPECT_FALSE(latest->response["persons"][1].contains("posture"));
  EXPECT_EQ(latest->response["persons"][1]["posture_source"], "yolo");

  store.SetSource(Pose(2), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(2, humans));
  latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->response["persons"][0]["posture_source"], "yolo");
  EXPECT_GE(latest->response["persons"][0]["posture_age_ms"].get<int>(), 0);
  EXPECT_FALSE(store.SetGemmaPosture(3, "1", "sitting", "none"));

  store.SetSource(Pose(3), false, false, false, false);
  humans["persons"].erase(humans["persons"].begin());
  humans["person_count"] = 1;
  ASSERT_TRUE(store.SetHuman(3, humans));
  EXPECT_FALSE(store.SetGemmaPosture(1, "1", "standing", "none"));
  EXPECT_EQ(store.Latest()->response["persons"][0]["posture_source"], "yolo");
  store.SetSource(Pose(4), false, false, false, false);
  humans["persons"] = {{{"track_id", 1}, {"bbox", {0, 0, 12, 12}},
                         {"confidence", 0.9}, {"posture_state", "standing"}}};
  ASSERT_TRUE(store.SetHuman(4, humans));
  EXPECT_FALSE(store.SetGemmaPosture(1, "1", "lying", "fallen"));
  EXPECT_EQ(store.Latest()->response["persons"][0]["posture_source"], "yolo");
}

TEST(CameraPoseStoreTest, CompatibleGemmaPostureDoesNotOverrideYoloWaving) {
  CameraPoseStore store;
  nlohmann::json humans = {{"persons", {{{"track_id", 1}, {"bbox", {0, 0, 12, 12}},
                                         {"confidence", 0.9}, {"posture_state", "standing"},
                                         {"posture", "standing"}, {"pose", "stand"}}}},
                           {"person_count", 1}};
  store.SetSource(Pose(1), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(1, humans));
  ASSERT_TRUE(store.SetGemmaPosture(1, "1", "standing", "waving"));
  store.SetSource(Pose(2), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(2, humans));
  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->response["persons"][0]["posture_state"], "standing");
  EXPECT_EQ(latest->response["persons"][0]["action"], "unknown");
  EXPECT_EQ(latest->response["persons"][0]["posture_source"], "gemma4");
  EXPECT_EQ(latest->response["persons"][0]["action_source"], "yolo");

  humans["persons"][0]["action"] = "waving";
  store.SetSource(Pose(3), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(3, humans));
  const auto waved = store.Latest();
  ASSERT_TRUE(waved);
  EXPECT_EQ(waved->response["persons"][0]["action"], "waving");
  EXPECT_EQ(waved->response["persons"][0]["action_source"], "yolo");
}

TEST(CameraPoseStoreTest, ActionLeavesFurnitureAndFallSemanticsToWorldProjection) {
  struct Case { const char* pose; const char* posture; const char* raw_action; const char* expected; };
  const std::vector<Case> cases = {
      {"lie", "lying_on_floor", "waving", "unknown"},
      {"lie", "lying_on_furniture", "waving", "unknown"},
      {"sit", "sitting_on_furniture", "none", "unknown"},
      {"sit", "sitting", "waving", "waving"},
      {"stand", "standing", "waving", "waving"},
      {"unknown", "unknown", "waving", "waving"},
      {"stand", "standing", "none", "unknown"},
  };
  for (const auto& test : cases) {
    CameraPoseStore store;
    store.SetSource(Pose(1), false, false, false, false);
    ASSERT_TRUE(store.SetHuman(1, {{"persons", {{{"track_id", 1},
                                                 {"bbox", {0, 0, 12, 12}},
                                                 {"pose", test.pose},
                                                 {"posture_state", test.posture},
                                                 {"action", test.raw_action}}}}}));
    EXPECT_EQ(store.Latest()->response["persons"][0]["action"], test.expected);
    EXPECT_EQ(store.Latest()->response["persons"][0]["pose"], test.pose);
  }
}

TEST(CameraPoseStoreTest, RejectsGemmaResultAfterYoloContextChangesAndReturns) {
  CameraPoseStore store;
  auto publish = [&](std::uint64_t generation, const char* pose) {
    store.SetSource(Pose(generation), false, false, false, false);
    ASSERT_TRUE(store.SetHuman(generation, {{"persons", {{{"track_id", 1},
        {"bbox", {0, 0, 12, 12}}, {"pose", pose}, {"action", "none"}}}}}));
  };
  publish(1, "sit");
  const auto original_revision = store.TrackInputRevision("1");
  publish(2, "stand");
  publish(3, "sit");
  EXPECT_NE(store.TrackInputRevision("1"), original_revision);
  EXPECT_FALSE(store.SetGemmaPosture(1, "1", "sitting_on_furniture", "none",
                                    original_revision));
}

TEST(CameraPoseStoreTest, ActionTransitionDoesNotAddAnotherHoldDelay) {
  CameraPoseStore store;
  store.SetSource(Pose(1), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(1, {{"persons", {{{"track_id", 1}, {"bbox", {0, 0, 12, 12}},
      {"pose", "stand"}, {"action", "waving"}}}}}));
  EXPECT_EQ(store.Latest()->response["persons"][0]["action"], "waving");
  store.SetSource(Pose(2), false, false, false, false);
  ASSERT_TRUE(store.SetHuman(2, {{"persons", {{{"track_id", 1}, {"bbox", {0, 0, 12, 12}},
      {"pose", "sit"}, {"action", "none"}}}}}));
  EXPECT_EQ(store.Latest()->response["persons"][0]["action"], "unknown");
}

TEST(CameraPoseStoreTest, TracksIndependentModelStateForOneSource) {
  CameraPoseStore store;
  auto source = Pose(12);
  store.SetSource(std::move(source), true, true, true, true);

  auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->sequence, 1u);
  EXPECT_EQ(latest->human.status, CameraPoseStore::Status::kPending);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kPending);
  EXPECT_EQ(latest->action.status, CameraPoseStore::Status::kPending);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kPending);

  ASSERT_TRUE(store.SetHuman(12, {{"persons", nlohmann::json::array()}, {"person_count", 0}}));
  latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->sequence, 1u);
  EXPECT_EQ(latest->human.status, CameraPoseStore::Status::kComplete);
  EXPECT_EQ(latest->human.sequence, 1u);
  EXPECT_EQ(latest->human.source_generation, 12u);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kPending);

  ASSERT_TRUE(store.MergePose(12, {{"persons", nlohmann::json::array()}}));
  latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kComplete);
  EXPECT_EQ(latest->action.status, CameraPoseStore::Status::kComplete);
  EXPECT_EQ(latest->pose.sequence, 1u);
  EXPECT_EQ(latest->action.sequence, 1u);
}

TEST(CameraPoseStoreTest, PublishesUnavailableModelStatesIndependently) {
  CameraPoseStore store;
  store.SetSource(Pose(3), true, true, true, true);
  EXPECT_TRUE(store.SetHumanUnavailable(3));
  EXPECT_TRUE(store.SetPoseUnavailable(3));
  store.SetFaceUnavailable(3);

  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->human.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->action.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->sequence, 1u);
}

TEST(CameraPoseStoreTest, HumanFailureTerminatesDependentPendingModels) {
  CameraPoseStore store;
  int updates = 0;
  store.SetUpdatedCallback([&] { ++updates; });
  store.SetSource(Pose(6), true, true, true, true);
  ASSERT_TRUE(store.SetHumanUnavailable(6));

  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->human.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->action.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(updates, 2);
}

TEST(CameraPoseStoreTest, NewSourceRetainsFreshFaceObservationWhileNextCallIsPending) {
  CameraPoseStore store;
  store.SetSource(Pose(1), false, false, true, true);
  ASSERT_TRUE(store.SetHuman(1, {{"persons", nlohmann::json::array()}, {"person_count", 0}}));
  store.SetFace(1, {{"data", {{"faces", nlohmann::json::array()}}}});

  store.SetSource(Pose(2), false, false, true, true);
  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->generation, 2u);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kPending);
  EXPECT_EQ(latest->face.sequence, 1u);
  ASSERT_TRUE(latest->face_response);
  EXPECT_EQ(latest->face_generation, 1u);
}

TEST(CameraPoseStoreTest, RetainsCompletedFieldsAndProvenanceDuringSlowerRequests) {
  CameraPoseStore store;
  store.SetSource(Pose(1), true, true, true, true);
  ASSERT_TRUE(store.SetHuman(
      1, {{"persons", {{{"track_id", 7}, {"bbox", {1, 2, 3, 4}}, {"confidence", 0.9}}}},
          {"person_count", 1}}));
  ASSERT_TRUE(store.MergePose(
      1, {{"persons", {{{"track_id", 7}, {"pose", "stand"}, {"action", "none"}}}}}));

  store.SetSource(Pose(2), true, false, true, false);
  ASSERT_TRUE(store.SetHuman(
      2, {{"persons", {{{"track_id", 7}, {"bbox", {2, 3, 4, 5}}, {"confidence", 0.8}}}},
          {"person_count", 1}}));
  auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kComplete);
  EXPECT_EQ(latest->pose.source_generation, 1u);
  EXPECT_EQ(latest->pose.requested_generation, 1u);
  EXPECT_EQ(latest->response.at("persons").at(0).at("pose"), "stand");
  EXPECT_EQ(latest->response.at("persons").at(0).at("action"), "unknown");

  store.SetSource(Pose(3), true, true, true, false);
  latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kPending);
  EXPECT_EQ(latest->pose.source_generation, 1u);
  EXPECT_EQ(latest->pose.requested_generation, 3u);
  EXPECT_EQ(latest->response.at("persons").at(0).at("pose"), "stand");
}

TEST(CameraPoseStoreTest, ConfiguredIdleModelsUseUnavailablePlaceholderState) {
  CameraPoseStore store;
  store.SetSource(Pose(1), true, false, true, false);

  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->pose.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->action.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kUnavailable);
  EXPECT_EQ(latest->pose.requested_generation, 0u);
  EXPECT_EQ(latest->face.requested_generation, 0u);
}

TEST(CameraPoseStoreTest, FaceFailureLeavesImmediatelyPublishedPoseAvailable) {
  CameraPoseStore store;
  store.SetPose(Pose(7), true);
  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 7);
  store.SetFaceUnavailable(7);

  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 7);
  EXPECT_FALSE(store.Latest()->face_response.has_value());
}

TEST(CameraPoseStoreTest, LateFaceResultEnrichesNewestPoseFromItsSourceGeneration) {
  CameraPoseStore store;
  store.SetPose(Pose(10), true);
  store.SetPose(Pose(11), true);

  store.SetFace(10, {{"data", {{"faces", nlohmann::json::array()}}}});

  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->generation, 11);
  EXPECT_EQ(store.Latest()->sequence, 2);
  EXPECT_EQ(store.Latest()->face_generation, 10);
  EXPECT_EQ(store.Latest()->face_pose_response, Pose(10).response);
}

TEST(CameraPoseStoreTest, EarlyFaceResultWaitsForMatchingHumanTracks) {
  CameraPoseStore store;
  store.SetSource(Pose(10), false, false, true, true);
  store.SetFace(10, {{"data", {{"faces", nlohmann::json::array()}}}});

  auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->face.status, CameraPoseStore::Status::kComplete);
  EXPECT_FALSE(latest->face_response);

  ASSERT_TRUE(store.SetHuman(
      10, {{"persons", {{{"track_id", 7}, {"bbox", {1, 2, 3, 4}}, {"confidence", 0.9}}}},
           {"person_count", 1}}));
  latest = store.Latest();
  ASSERT_TRUE(latest);
  ASSERT_TRUE(latest->face_response);
  ASSERT_TRUE(latest->face_pose_response);
  EXPECT_EQ(latest->face_generation, 10u);
  EXPECT_EQ(latest->face_pose_response->at("persons").at(0).at("track_id"), 7);
}

TEST(CameraPoseStoreTest, RetainsFourExactFramesBySequenceAndGeneration) {
  CameraPoseStore store;
  for (std::uint64_t generation = 1; generation <= 5; ++generation) {
    auto snapshot = Pose(generation);
    snapshot.jpeg = std::make_shared<const std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(generation)});
    store.Set(std::move(snapshot));
  }

  EXPECT_FALSE(store.Find("camera.front", 1, 1).has_value());
  for (std::uint64_t sequence = 2; sequence <= 5; ++sequence) {
    auto snapshot = store.Find("camera.front", sequence, sequence);
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(snapshot->jpeg);
    EXPECT_EQ(*snapshot->jpeg, std::vector<std::uint8_t>{static_cast<std::uint8_t>(sequence)});
  }
  EXPECT_FALSE(store.Find("camera.front", 5, 4).has_value());
  EXPECT_FALSE(store.Find("camera.claimed", 5, 5).has_value());
}

TEST(CameraPoseStoreTest, ExactFrameRetainsAlignedDepthAndScale) {
  CameraPoseStore store;
  auto snapshot = Pose(8);
  snapshot.jpeg =
      std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{1, 2});
  snapshot.depth_png =
      std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{3, 4});
  snapshot.depth_scale = 0.00025;
  store.Set(std::move(snapshot));

  auto exact = store.Find("camera.front", 1, 8);
  ASSERT_TRUE(exact);
  ASSERT_TRUE(exact->jpeg);
  ASSERT_TRUE(exact->depth_png);
  EXPECT_EQ(exact->device_id, "camera.front");
  EXPECT_EQ(*exact->jpeg, (std::vector<std::uint8_t>{1, 2}));
  EXPECT_EQ(*exact->depth_png, (std::vector<std::uint8_t>{3, 4}));
  EXPECT_DOUBLE_EQ(exact->depth_scale, 0.00025);
  EXPECT_FALSE(store.Find("camera.front", 1, 9));
}

TEST(CameraPoseStoreTest, RawDepthHistoryIsBoundedWithExactFrames) {
  CameraPoseStore store;
  for (std::uint64_t generation = 1; generation <= 6; ++generation) {
    auto snapshot = Pose(generation);
    snapshot.depth_u16 = std::make_shared<const std::vector<std::uint16_t>>(
        std::vector<std::uint16_t>{static_cast<std::uint16_t>(generation)});
    store.Set(std::move(snapshot));
  }
  EXPECT_FALSE(store.Find("camera.front", 2, 2));
  for (std::uint64_t generation = 3; generation <= 6; ++generation) {
    const auto found = store.Find("camera.front", generation, generation);
    ASSERT_TRUE(found);
    ASSERT_TRUE(found->depth_u16);
    EXPECT_EQ(found->depth_u16->front(), generation);
  }
}

TEST(CameraPoseTest, LazilyEncodesRawAlignedDepth) {
  const auto png = EncodeAlignedDepthPng({1000, 2000}, 2, 1);
  ASSERT_GE(png.size(), 8u);
  EXPECT_EQ(png[0], 0x89);
  EXPECT_EQ(png[1], 'P');
  EXPECT_TRUE(EncodeAlignedDepthPng({1000}, 2, 1).empty());
}

TEST(CameraPoseTest, AddsRobustDepthForValidJointsAndNullForInvalidJoints) {
  hardware::CameraTelemetry frame;
  frame.width_px = 8;
  frame.height_px = 8;
  frame.depth_scale = 0.001;
  frame.depth_u16.assign(64, 1500);
  frame.depth_u16[3 * 8 + 3] = 0;
  nlohmann::json points = nlohmann::json::array();
  nlohmann::json scores = nlohmann::json::array();
  for (int index = 0; index < 17; ++index) {
    points.push_back(index == 2 ? nlohmann::json({20.0, 20.0}) : nlohmann::json({3.0, 3.0}));
    scores.push_back(index == 1 ? 0.1 : 0.9);
  }
  nlohmann::json response = {
      {"persons", {{{"track_id", 1}, {"keypoints", points}, {"keypoint_scores", scores}}}}};
  AddAlignedKeypointDepths(response, frame);
  const auto& depths = response.at("persons").at(0).at("keypoint_depth_m");
  ASSERT_EQ(depths.size(), 17u);
  EXPECT_DOUBLE_EQ(depths.at(0), 1.5);
  EXPECT_TRUE(depths.at(1).is_null());
  EXPECT_TRUE(depths.at(2).is_null());
}

TEST(CameraPoseStoreTest, RepublishingKeepsCompletedPoseSourceAssociation) {
  CameraPoseStore store;
  auto pose = Pose(42);
  pose.frame_id = "pose-frame-42";
  pose.source_timestamp_ns = 123456;
  pose.jpeg = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>{4, 2});
  store.SetPose(std::move(pose));

  ASSERT_TRUE(store.RepublishLatest());
  const auto latest = store.Latest();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->sequence, 2);
  EXPECT_EQ(latest->generation, 42);
  EXPECT_EQ(latest->frame_id, "pose-frame-42");
  EXPECT_EQ(latest->device_id, "camera.front");
  EXPECT_EQ(latest->source_timestamp_ns, 123456);
  ASSERT_TRUE(latest->jpeg);
  EXPECT_EQ(*latest->jpeg, (std::vector<std::uint8_t>{4, 2}));
  EXPECT_TRUE(store.Find("camera.front", 2, 42).has_value());
  EXPECT_FALSE(store.Find("camera.front", 2, 41).has_value());
}

TEST(CameraPoseStoreTest, CannotRepublishBeforeFirstCompletedPose) {
  CameraPoseStore store;
  EXPECT_FALSE(store.RepublishLatest());
  EXPECT_FALSE(store.Latest().has_value());
}

TEST(CameraPoseStoreTest, AllocatesMonotonicGenerationsAcrossPumpInstances) {
  CameraPoseStore store;
  EXPECT_EQ(store.NextGeneration(), 1u);
  store.Set(Pose(7));
  EXPECT_EQ(store.NextGeneration(), 8u);
  EXPECT_EQ(store.NextGeneration(), 9u);
}

TEST(CameraPoseStoreTest, PoseMergesCompletedGenerationIntoCurrentKnownTracks) {
  CameraPoseStore store;
  auto detection = Pose(4);
  detection.response = {
      {"persons",
       {{{"track_id", 7}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.8}, {"distance_m", 1.25}}}},
      {"person_count", 1}};
  store.SetDetection(std::move(detection));

  EXPECT_FALSE(store.MergePose(5, {{"persons", nlohmann::json::array()}, {"person_count", 0}}));
  EXPECT_FALSE(store.MergePose(3, {{"persons", nlohmann::json::array()}, {"person_count", 0}}));
  EXPECT_TRUE(store.MergePose(3, {{"persons",
                                   {{{"track_id", 7},
                                     {"bbox", {9, 9, 9, 9}},
                                     {"confidence", 0.1},
                                     {"distance_m", nullptr},
                                     {"pose", "lie"},
                                     {"posture_state", "lying_on_floor"},
                                     {"action", "fallen"}},
                                    {{"track_id", 99}, {"posture_state", "lying"}}}},
                                  {"person_count", 2}}));

  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  ASSERT_EQ(latest->response.at("persons").size(), 1);
  const auto& person = latest->response.at("persons").at(0);
  EXPECT_EQ(person.at("track_id"), 7);
  EXPECT_EQ(person.at("bbox"), nlohmann::json({1, 2, 30, 40}));
  EXPECT_EQ(person.at("confidence"), 0.8);
  EXPECT_EQ(person.at("distance_m"), 1.25);
  EXPECT_EQ(person.at("pose"), "lie");
  EXPECT_EQ(person.at("posture_state"), "lying_on_floor");
  EXPECT_EQ(person.at("action"), "unknown");
}

TEST(CameraPoseStoreTest, NewHumanDistanceReplacesRetainedEnrichmentDistance) {
  CameraPoseStore store;
  auto detection = Pose(4);
  detection.response = {{"persons",
                         {{{"track_id", 7},
                           {"bbox", {1, 2, 30, 40}},
                           {"confidence", 0.8},
                           {"distance_m", 1.25},
                           {"posture_state", "standing"}}}},
                        {"person_count", 1}};
  store.SetDetection(std::move(detection));

  store.SetSource(Pose(5), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      5, {{"persons",
           {{{"track_id", 7}, {"bbox", {2, 3, 31, 41}}, {"confidence", 0.9}, {"distance_m", 2.5}}}},
          {"person_count", 1}}));

  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  const auto& person = latest->response.at("persons").at(0);
  EXPECT_EQ(person.at("distance_m"), 2.5);
  EXPECT_EQ(person.at("posture_state"), "standing");
}

TEST(CameraPoseStoreTest, RejectsPoseResultAfterTrackIdIsReused) {
  CameraPoseStore store;
  store.SetSource(Pose(1), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      1, {{"persons", {{{"track_id", 7}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.8}}}},
          {"person_count", 1}}));

  store.SetSource(Pose(2), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(2, {{"persons", nlohmann::json::array()}, {"person_count", 0}}));
  store.SetSource(Pose(3), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      3, {{"persons", {{{"track_id", 7}, {"bbox", {50, 60, 90, 120}}, {"confidence", 0.9}}}},
          {"person_count", 1}}));

  EXPECT_FALSE(store.MergePose(
      1, {{"persons", {{{"track_id", 7}, {"pose", "stand"}, {"action", "waving"},
                         {"action_confidence", 0.9}}}},
          {"person_count", 1}}));
  const auto latest = store.Latest();
  ASSERT_TRUE(latest);
  EXPECT_EQ(latest->response["persons"][0]["bbox"], nlohmann::json({50, 60, 90, 120}));
  EXPECT_EQ(latest->response["persons"][0]["action"], "unknown");
}

TEST(CameraPoseStoreTest, HumanFramesRetainWavingUntilNextPoseResult) {
  CameraPoseStore store;
  store.SetSource(Pose(1), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      1, {{"persons", {{{"track_id", 7}, {"bbox", {1, 2, 30, 40}}, {"confidence", 0.8}}}},
          {"person_count", 1}}));
  ASSERT_TRUE(store.MergePose(
      1, {{"persons", {{{"track_id", 7}, {"pose", "stand"}, {"action", "waving"},
                         {"action_confidence", 0.9}}}},
          {"person_count", 1}}));
  ASSERT_EQ(store.Latest()->response["persons"][0]["action"], "waving");

  store.SetSource(Pose(2), true, true, false, false);
  ASSERT_TRUE(store.SetHuman(
      2, {{"persons", {{{"track_id", 7}, {"bbox", {2, 3, 31, 41}}, {"confidence", 0.85}}}},
          {"person_count", 1}}));
  EXPECT_EQ(store.Latest()->response["persons"][0]["action"], "waving");
  EXPECT_EQ(store.Latest()->response["persons"][0]["action_confidence"], 0.9);

  ASSERT_TRUE(store.MergePose(
      2, {{"persons", {{{"track_id", 7}, {"pose", "stand"}, {"action", "none"},
                         {"action_confidence", 0.0}}}},
          {"person_count", 1}}));
  EXPECT_EQ(store.Latest()->response["persons"][0]["action"], "unknown");
  EXPECT_FALSE(store.Latest()->response["persons"][0].contains("action_confidence"));
}

TEST(CameraPosePumpTest, AcceptsHttpHumanModel) {
  ModelConfig model;
  model.id = "human";
  model.capability = Capability::kHuman;
  model.endpoint = "http://127.0.0.1:19001/infer";
  model.transport = Transport::kHttp;
  Runtime runtime({model});
  CameraPoseStore store;

  EXPECT_NO_THROW({
    CameraPosePump pump(
        runtime, [] { return std::optional<hardware::CameraTelemetry>{}; }, store, "human",
        "camera.front", std::chrono::milliseconds(10));
  });
}

TEST(CameraPosePumpTest, CompletesActiveInferenceAndCoalescesToLatestWaitingFrame) {
  DelayedSequenceServer server(std::chrono::milliseconds(100));
  ModelConfig model;
  model.id = "human";
  model.capability = Capability::kHuman;
  model.endpoint = "http://127.0.0.1:" + std::to_string(server.port()) + "/infer";
  model.transport = Transport::kHttp;
  model.timeout = std::chrono::seconds(2);
  Runtime runtime(
      {model}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};
  std::atomic<std::uint64_t> completed{0};
  store.SetUpdatedCallback([&] {
    const auto latest = store.Latest();
    if (latest && latest->human.status == CameraPoseStore::Status::kComplete) ++completed;
  });

  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          const auto sequence = ++captured;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = static_cast<std::int64_t>(sequence);
          frame.width_px = 16;
          frame.height_px = 16;
          frame.jpeg = {static_cast<std::uint8_t>(sequence)};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(10));

    for (int attempt = 0; attempt < 300; ++attempt) {
      if (completed.load() >= 2) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto latest = store.Latest();
    ASSERT_TRUE(latest);
    EXPECT_GE(completed.load(), 2u);
    EXPECT_GE(server.requests(), 2u);
    EXPECT_GE(latest->human.sequence, 2u);
    EXPECT_GE(latest->human.source_generation, 5u);
  }
}

TEST(CameraPosePumpTest, SendsRgbOnlyAndAddsRobustLocalDepthDistance) {
  DelayedSequenceServer server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":1,"bbox":[0,0,8,8],"confidence":0.9}],"person_count":1})");
  ModelConfig model;
  model.id = "human";
  model.capability = Capability::kHuman;
  model.endpoint = "http://127.0.0.1:" + std::to_string(server.port()) + "/infer";
  model.transport = Transport::kHttp;
  Runtime runtime(
      {model}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> sequence{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++sequence;
          frame.width_px = 8;
          frame.height_px = 8;
          frame.jpeg = {1, 2, 3};
          frame.depth_u16.assign(64, 1250);
          frame.depth_scale = 0.001;
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20));
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto latest = store.Latest();
      if (latest && latest->human.status == CameraPoseStore::Status::kComplete) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto latest = store.Latest();
    ASSERT_TRUE(latest);
    ASSERT_EQ(latest->response.at("persons").size(), 1u);
    EXPECT_DOUBLE_EQ(latest->response.at("persons").at(0).at("distance_m"), 1.25);
    const auto request = nlohmann::json::parse(server.last_request_body());
    EXPECT_TRUE(request.contains("input"));
    EXPECT_FALSE(request.contains("depth_input"));
  }
}

TEST(CameraPosePumpTest, SendsAlignedDepthToPoseAtMostFiveTimesPerSecond) {
  DelayedSequenceServer human_server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":1,"bbox":[0,0,8,8],"confidence":0.9}],"person_count":1})");
  DelayedSequenceServer pose_server(std::chrono::milliseconds(0));
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  human.transport = Transport::kHttp;
  ModelConfig pose = human;
  pose.id = "pose";
  pose.capability = Capability::kHumanPose;
  pose.endpoint = "http://127.0.0.1:" + std::to_string(pose_server.port()) + "/infer";
  Runtime runtime(
      {human, pose}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> sequence{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++sequence;
          frame.width_px = 8;
          frame.height_px = 8;
          frame.jpeg = {1, 2, 3};
          frame.depth_u16.assign(64, 1250);
          frame.depth_scale = 0.002;
          frame.depth_intrinsics = hardware::CameraIntrinsics{
              .width_px = 8,
              .height_px = 8,
              .fx = 6.0,
              .fy = 6.0,
              .ppx = 4.0,
              .ppy = 4.0,
              .distortion_model = "inverse_brown_conrady"};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20), {}, "pose");
    pump.SetPoseEnabled(true);
    for (int attempt = 0; attempt < 100 && pose_server.requests() < 8; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto bodies = pose_server.request_bodies();
  ASSERT_GE(bodies.size(), 8u);
  std::size_t depth_count = 0;
  for (const auto& body : bodies) {
    const auto request = nlohmann::json::parse(body);
    if (!request.contains("depth_input")) continue;
    ++depth_count;
    EXPECT_EQ(request.at("depth_input").at("media_type"), "image/png");
    EXPECT_EQ(request.at("depth_input").at("filename"), "camera-depth.png");
    EXPECT_EQ(request.at("depth_scale"), 0.002);
    ASSERT_TRUE(request.contains("depth_intrinsics"));
    EXPECT_EQ(request.at("depth_intrinsics").at("width"), 8);
    EXPECT_EQ(request.at("depth_intrinsics").at("height"), 8);
    EXPECT_EQ(request.at("depth_intrinsics").at("fx"), 6.0);
    EXPECT_EQ(request.at("depth_intrinsics").at("coeffs"),
              nlohmann::json({0.0, 0.0, 0.0, 0.0, 0.0}));
    EXPECT_EQ(request.at("depth_input").at("data_b64").get<std::string>().substr(0, 8), "iVBORw0K");
  }
  EXPECT_EQ(depth_count, 1u);
  EXPECT_LT(depth_count, bodies.size());
}

TEST(CameraPosePumpTest, OversizedDepthFallsBackToRgbPoseRequest) {
  DelayedSequenceServer human_server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":1,"bbox":[0,0,8,8],"confidence":0.9}],"person_count":1})");
  DelayedSequenceServer pose_server(std::chrono::milliseconds(0));
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  human.transport = Transport::kHttp;
  ModelConfig pose = human;
  pose.id = "pose";
  pose.capability = Capability::kHumanPose;
  pose.endpoint = "http://127.0.0.1:" + std::to_string(pose_server.port()) + "/infer";
  pose.max_request_bytes = 200;
  Runtime runtime(
      {human, pose}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> sequence{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++sequence;
          frame.width_px = 8;
          frame.height_px = 8;
          frame.jpeg = {1, 2, 3};
          frame.depth_u16.assign(64, 1250);
          frame.depth_scale = 0.001;
          frame.depth_intrinsics = hardware::CameraIntrinsics{
              .width_px = 8,
              .height_px = 8,
              .fx = 6.0,
              .fy = 6.0,
              .ppx = 4.0,
              .ppy = 4.0,
              .distortion_model = "inverse_brown_conrady"};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20), {}, "pose");
    pump.SetPoseEnabled(true);
    for (int attempt = 0; attempt < 100 && pose_server.requests() == 0; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_GE(pose_server.requests(), 1u);
  const auto request = nlohmann::json::parse(pose_server.last_request_body());
  EXPECT_TRUE(request.contains("input"));
  EXPECT_FALSE(request.contains("depth_input"));
  EXPECT_FALSE(request.contains("depth_scale"));
  EXPECT_FALSE(request.contains("depth_intrinsics"));
}

TEST(CameraPosePumpTest, StartsFaceInferenceAfterATrackedPersonAppears) {
  DelayedSequenceServer human_server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":7,"bbox":[1,1,15,15],"confidence":0.9}],"person_count":1})");
  DelayedSequenceServer face_server(std::chrono::milliseconds(0),
                                    R"({"data":{"faces":[],"face_count":0}})");
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  human.transport = Transport::kHttp;
  ModelConfig face;
  face.id = "face";
  face.capability = Capability::kFace;
  face.endpoint = "http://127.0.0.1:" + std::to_string(face_server.port()) + "/infer";
  face.transport = Transport::kHttp;
  Runtime runtime(
      {human, face}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};

  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          const auto sequence = ++captured;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = static_cast<std::int64_t>(sequence);
          frame.width_px = 16;
          frame.height_px = 16;
          frame.jpeg = {static_cast<std::uint8_t>(sequence)};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20), "face");
    pump.SetFaceEnabled(true);

    for (int attempt = 0; attempt < 100; ++attempt) {
      if (face_server.requests() >= 1) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const auto latest = store.Latest();
    ASSERT_TRUE(latest);
    EXPECT_GE(face_server.requests(), 1u);
    EXPECT_EQ(latest->response.at("persons").size(), 1u);
    EXPECT_GE(human_server.requests(), 1u);
  }
}

TEST(CameraPosePumpTest, DoesNotSendArcFaceRequestsWhenNoHumanIsTracked) {
  DelayedSequenceServer human_server(std::chrono::milliseconds(0));
  DelayedSequenceServer face_server(std::chrono::milliseconds(0),
                                    R"({"data":{"faces":[],"face_count":0}})");
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  human.transport = Transport::kHttp;
  ModelConfig face;
  face.id = "face";
  face.capability = Capability::kFace;
  face.endpoint = "http://127.0.0.1:" + std::to_string(face_server.port()) + "/infer";
  face.transport = Transport::kHttp;
  Runtime runtime(
      {human, face}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++captured;
          frame.width_px = 16;
          frame.height_px = 16;
          frame.jpeg = {1, 2, 3};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20), "face");
    pump.SetFaceEnabled(true, std::chrono::milliseconds(250));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(face_server.requests(), 0u);
  }
}

TEST(CameraPosePumpTest, SlowsArcFaceRefreshAfterRegisteredIdentityIsResolved) {
  DelayedSequenceServer human_server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":7,"bbox":[1,1,15,15],"confidence":0.9}],"person_count":1})");
  DelayedSequenceServer face_server(
      std::chrono::milliseconds(0),
      R"({"status":"success","operation":"recognize","data":{"faces":[{"bbox":[2,2,8,8],"identity":"owner","known":true,"similarity":0.99}],"face_count":1}})");
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  human.transport = Transport::kHttp;
  ModelConfig face;
  face.id = "face";
  face.capability = Capability::kFace;
  face.endpoint = "http://127.0.0.1:" + std::to_string(face_server.port()) + "/infer";
  face.transport = Transport::kHttp;
  Runtime runtime(
      {human, face}, [](const std::string&, const nlohmann::json&) {},
      [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++captured;
          frame.width_px = 16;
          frame.height_px = 16;
          frame.jpeg = {1, 2, 3};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(10), "face");
    pump.SetFaceEnabled(true, std::chrono::milliseconds(250));
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto latest = store.Latest();
      if (face_server.requests() >= 1 && latest &&
          latest->face_identities.find("7") != latest->face_identities.end())
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(face_server.requests(), 1u);
    auto latest = store.Latest();
    ASSERT_TRUE(latest);
    ASSERT_NE(latest->face_identities.find("7"), latest->face_identities.end())
        << "persons=" << latest->response.dump()
        << " face=" << (latest->face_response ? latest->face_response->dump() : "none");
    EXPECT_EQ(latest->face_identities.at("7").identity, "owner");
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (face_server.requests() >= 2) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_GE(face_server.requests(), 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    EXPECT_EQ(face_server.requests(), 2u);
  }
}

TEST(CameraPosePumpTest, GemmaClassifiesStableLargestTrackWithoutBlockingHumanFrames) {
  DelayedSequenceServer human_server(
      std::chrono::milliseconds(0),
      R"({"persons":[{"track_id":1,"bbox":[0,0,4,4],"confidence":0.9,"posture_state":"standing"},{"track_id":2,"bbox":[0,0,12,12],"confidence":0.8,"posture_state":"standing"}],"person_count":2})");
  DelayedSequenceServer gemma_server(std::chrono::milliseconds(250),
                                     R"({"posture_state":"sitting","action":"waving"})");
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  ModelConfig gemma;
  gemma.id = "gemma";
  gemma.capability = Capability::kPostureClassification;
  gemma.endpoint = "http://127.0.0.1:" + std::to_string(gemma_server.port()) + "/infer";
  gemma.timeout = std::chrono::seconds(2);
  Runtime runtime({human, gemma}, [](const std::string&, const nlohmann::json&) {},
                  [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};
  {
    CameraPosePump pump(
        runtime,
        [&] {
          hardware::CameraTelemetry frame;
          frame.frame_id = "camera";
          frame.source_timestamp_ns = ++captured;
          frame.width_px = 16;
          frame.height_px = 16;
          frame.jpeg = {static_cast<std::uint8_t>(captured)};
          return std::optional<hardware::CameraTelemetry>(std::move(frame));
        },
        store, "human", "camera.front", std::chrono::milliseconds(20), {}, {}, "gemma");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(gemma_server.requests(), 0u);
    for (int attempt = 0; attempt < 200 && gemma_server.requests() == 0; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(gemma_server.requests(), 1u);
    EXPECT_GE(human_server.requests(), 8u);
    const auto human_at_gemma_start = human_server.requests();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_GT(human_server.requests(), human_at_gemma_start);
    EXPECT_EQ(gemma_server.requests(), 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto latest = store.Latest();
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->response["persons"][0]["posture_source"], "yolo");
    EXPECT_EQ(latest->response["persons"][1]["posture_state"], "standing");
    EXPECT_EQ(latest->response["persons"][1]["action"], "unknown");
    EXPECT_EQ(latest->response["persons"][1]["action_source"], "yolo");
    EXPECT_EQ(latest->response["persons"][1]["posture_source"], "yolo");
    EXPECT_EQ(gemma_server.requests(), 1u);
    const auto request = nlohmann::json::parse(gemma_server.last_request_body());
    EXPECT_EQ(request["bbox"], nlohmann::json({0, 0, 12, 12}));
    human_server.SetBody(
        R"({"persons":[{"track_id":3,"bbox":[1,1,14,14],"confidence":0.9,"posture_state":"standing"}],"person_count":1})");
    for (int attempt = 0; attempt < 200 && gemma_server.requests() < 2; ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(gemma_server.requests(), 2u);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto replacement = store.Latest();
    ASSERT_TRUE(replacement);
    ASSERT_EQ(replacement->response["persons"].size(), 1u);
    EXPECT_EQ(replacement->response["persons"][0]["track_id"], 3);
    EXPECT_EQ(replacement->response["persons"][0]["posture_source"], "yolo");
  }
}

TEST(CameraPosePumpTest, GemmaWaitsForStableBoundingBox) {
  const std::string left =
      R"({"persons":[{"track_id":1,"bbox":[0,0,12,12],"confidence":0.9}],"person_count":1})";
  const std::string right =
      R"({"persons":[{"track_id":1,"bbox":[4,0,16,12],"confidence":0.9}],"person_count":1})";
  std::vector<std::string> responses;
  for (int i = 0; i < 12; ++i) responses.push_back(i % 2 == 0 ? left : right);
  responses.push_back(left);
  DelayedSequenceServer human_server(std::chrono::milliseconds(0), std::move(responses));
  DelayedSequenceServer gemma_server(std::chrono::milliseconds(0),
                                     R"({"posture_state":"standing","action":"none"})");
  ModelConfig human;
  human.id = "human";
  human.capability = Capability::kHuman;
  human.endpoint = "http://127.0.0.1:" + std::to_string(human_server.port()) + "/infer";
  ModelConfig gemma;
  gemma.id = "gemma";
  gemma.capability = Capability::kPostureClassification;
  gemma.endpoint = "http://127.0.0.1:" + std::to_string(gemma_server.port()) + "/infer";
  Runtime runtime({human, gemma}, [](const std::string&, const nlohmann::json&) {},
                  [](const ModelConfig&) { return std::make_shared<TestJsonAdapter>(); });
  CameraPoseStore store;
  std::atomic<std::uint64_t> captured{0};
  CameraPosePump pump(
      runtime,
      [&] {
        hardware::CameraTelemetry frame;
        frame.frame_id = "camera";
        frame.source_timestamp_ns = ++captured;
        frame.width_px = 16;
        frame.height_px = 16;
        frame.jpeg = {static_cast<std::uint8_t>(captured)};
        return std::optional<hardware::CameraTelemetry>(std::move(frame));
      },
      store, "human", "camera.front", std::chrono::milliseconds(20), {}, {}, "gemma");
  for (int attempt = 0; attempt < 200 && human_server.requests() < 12; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  ASSERT_GE(human_server.requests(), 12u);
  EXPECT_EQ(gemma_server.requests(), 0u);
  for (int attempt = 0; attempt < 200 && gemma_server.requests() == 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(gemma_server.requests(), 1u);
}

}  // namespace
}  // namespace eos::runtime::models

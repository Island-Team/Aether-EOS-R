#include <gtest/gtest.h>

#include <limits>

#include "detection_overlay.h"

namespace {

using eos::runtime::perception::body::DetectionOverlayStore;

nlohmann::json Overlay() {
  return {{"source", "camera-1"},
          {"source_generation", 5u},
          {"target_id", "human:7"},
          {"target_bbox", {10, 20, 50, 80}},
          {"detection", {{"kind", "human"},
                         {"label", "person"},
                         {"bbox", {10, 20, 50, 80}},
                         {"distance_m", 1.2},
                         {"keypoint_depth_m", nlohmann::json::array({1.2})},
                         {"attributes", {{"pose", "lie"},
                                         {"action", "fallen"},
                                         {"activity", "fallen"},
                                         {"name", "Alice"}}}}}};
}

nlohmann::json Snapshot(unsigned generation, nlohmann::json detections = nlohmann::json::array()) {
  return {{"source", "camera-1"},
          {"source_generation", generation},
          {"detections", std::move(detections)}};
}

TEST(DetectionOverlayTest, InsertsOnlyForExactSourceGeneration) {
  DetectionOverlayStore overlays;
  overlays.Upsert("execution-1", Overlay());
  auto exact = Snapshot(5);
  overlays.Apply(&exact);
  ASSERT_EQ(exact["detections"].size(), 1u);
  EXPECT_EQ(exact["detections"][0]["id"], "human:7");
  EXPECT_EQ(exact["detections"][0]["attributes"]["pose"], "lie");

  auto prior = Snapshot(4, nlohmann::json::array({
      {{"id", "human:7"}, {"kind", "human"}, {"bbox", {10, 20, 50, 80}},
       {"attributes", {{"pose", "stand"}}}}}));
  overlays.Apply(&prior);
  EXPECT_EQ(prior["detections"][0]["attributes"]["pose"], "stand");

  auto future = Snapshot(6);
  overlays.Apply(&future);
  EXPECT_TRUE(future["detections"].empty());
  auto wrong_source = Snapshot(5);
  wrong_source["source"] = "camera-2";
  overlays.Apply(&wrong_source);
  EXPECT_TRUE(wrong_source["detections"].empty());
}

TEST(DetectionOverlayTest, ReplacesMatchedHumanAndPreservesTrack) {
  DetectionOverlayStore overlays;
  overlays.Upsert("execution-1", Overlay());
  auto snapshot = Snapshot(6, nlohmann::json::array({
      {{"id", "human:changed"}, {"kind", "human"}, {"bbox", {12, 22, 52, 82}},
       {"attributes", {{"pose", "unknown"}, {"speaking", true}}}},
      {{"id", "human:other"}, {"kind", "human"}, {"bbox", {200, 200, 300, 300}},
       {"attributes", {{"pose", "stand"}}}}}));
  overlays.Apply(&snapshot);
  ASSERT_EQ(snapshot["detections"].size(), 2u);
  EXPECT_EQ(snapshot["detections"][0]["id"], "human:changed");
  EXPECT_EQ(snapshot["detections"][0]["attributes"]["pose"], "lie");
  EXPECT_EQ(snapshot["detections"][0]["attributes"]["name"], "Alice");
  EXPECT_TRUE(snapshot["detections"][0]["attributes"]["speaking"]);
  EXPECT_EQ(snapshot["detections"][0]["bbox"], nlohmann::json::array({12, 22, 52, 82}));
  EXPECT_FALSE(snapshot["detections"][0].contains("keypoint_depth_m"));
  EXPECT_EQ(snapshot["detections"][1]["attributes"]["pose"], "stand");
  overlays.Clear("execution-1");
  auto cleared = Snapshot(6, nlohmann::json::array({
      {{"id", "human:changed"}, {"kind", "human"}, {"bbox", {12, 22, 52, 82}},
       {"attributes", {{"pose", "unknown"}}}}}));
  overlays.Apply(&cleared);
  EXPECT_EQ(cleared["detections"][0]["attributes"]["pose"], "unknown");
}

TEST(DetectionOverlayTest, CarriesAlignedGeometryAtActivationThenUsesFreshFrames) {
  DetectionOverlayStore overlays;
  auto overlay = Overlay();
  overlay["source_gimbal"] = {{"pan_rad", 0.1}, {"tilt_rad", -0.8}};
  overlay["activation_generation"] = 7u;
  overlays.Upsert("execution-1", overlay);
  auto matched = Snapshot(7, nlohmann::json::array({
      {{"id", "human:7"}, {"kind", "human"}, {"bbox", {12, 22, 52, 82}},
       {"attributes", {{"pose", "unknown"}}}}}));
  matched["source_gimbal"] = {{"pan_rad", 0.11}, {"tilt_rad", -0.79}};
  overlays.Apply(&matched);
  EXPECT_EQ(matched["detections"][0]["bbox"], nlohmann::json::array({10, 20, 50, 80}));
  EXPECT_EQ(matched["detections"][0]["keypoint_depth_m"], nlohmann::json::array({1.2}));

  auto still_down = Snapshot(8, nlohmann::json::array({
      {{"id", "human:7"}, {"kind", "human"}, {"bbox", {12, 22, 52, 82}},
       {"attributes", {{"pose", "unknown"}}}}}));
  still_down["source_gimbal"] = {{"pan_rad", 0.1}, {"tilt_rad", -0.8}};
  overlays.Apply(&still_down);
  EXPECT_EQ(still_down["detections"][0]["bbox"], nlohmann::json::array({12, 22, 52, 82}));
  EXPECT_FALSE(still_down["detections"][0].contains("keypoint_depth_m"));

  auto moved = Snapshot(7, nlohmann::json::array({
      {{"id", "human:7"}, {"kind", "human"}, {"bbox", {12, 22, 52, 82}},
       {"attributes", {{"pose", "unknown"}}}}}));
  moved["source_gimbal"] = {{"pan_rad", 0.1}, {"tilt_rad", 0.0}};
  overlays.Apply(&moved);
  EXPECT_EQ(moved["detections"][0]["bbox"], nlohmann::json::array({12, 22, 52, 82}));
  EXPECT_FALSE(moved["detections"][0].contains("keypoint_depth_m"));
  EXPECT_EQ(moved["detections"][0]["attributes"]["pose"], "lie");
}

TEST(DetectionOverlayTest, RejectsIncompleteOrWrongSemanticCorrection) {
  DetectionOverlayStore overlays;
  auto overlay = Overlay();
  overlay.erase("source_generation");
  EXPECT_THROW(overlays.Upsert("execution", overlay), std::invalid_argument);
  overlay = Overlay();
  overlay["detection"]["attributes"]["pose"] = "stand";
  EXPECT_THROW(overlays.Upsert("execution", overlay), std::invalid_argument);
  overlay = Overlay();
  overlay["target_bbox"][0] = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(overlays.Upsert("execution", overlay), std::invalid_argument);
}

}  // namespace

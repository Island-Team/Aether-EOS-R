/// Cross-module main-chain test: the committed full demo profile is
/// admitted by the bridge and projected into the compact EOS north-bound body,
/// detection surfaces. Raw camera capture is gateway-owned and is
/// deliberately outside this Reader-level test.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/eos/lib/clock.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/models/microphone_vad.h"
#include "src/eos/runtime/perception/body/reader.h"
#include "src/eos/runtime/perception/product_history.h"
#include "src/eos/runtime/perception/resource_catalog.h"

int main() {
  using namespace eos;

  runtime::hardware_runtime::Registry registry = runtime::hardware_runtime::Registry::Load(
      std::string(EOS_SOURCE_ETC) + "/backends/demo.toml", "demo");
  assert(registry.devices().size() == 7);

  lib::ManualClock clock;
  runtime::models::CameraPoseStore pose;
  runtime::models::CameraPoseStore::Snapshot snapshot;
  snapshot.model_id = "pose";
  snapshot.device_id =
      registry.Find(hardware::DeviceKind::kCamera).identity().device_id;
  snapshot.source_timestamp_ns = 12;
  snapshot.image_width_px = 1280;
  snapshot.image_height_px = 720;
  snapshot.response = {{"persons",
                        {{{"track_id", 7},
                          {"bbox", {0.0, 0.0, 100.0, 200.0}},
                          {"confidence", 0.94},
                           {"distance_m", 1.5},
                           {"posture_state", "standing"},
                           {"posture_confidence", 0.87},
                           {"behavior_event", "none"},
                           {"behavior_confidence", 0.0},
                           {"pose", "stand"},
                           {"pose_confidence", 0.87},
                           {"action", "none"},
                           {"action_confidence", 0.0},
                          {"keypoints", nlohmann::json::array()},
                          {"keypoint_scores", nlohmann::json::array()}}}},
                       {"person_count", 1}};
  pose.Set(std::move(snapshot));
  runtime::perception::body::Reader projection(registry, clock, &pose);

  // Every body projection is the same compact common health/state shape.
  for (const auto& device : registry.devices()) {
    const nlohmann::json body =
        projection.BuildDeviceState(device.robot_id(), device.identity().device_id);
    assert(body.contains("device_class"));
    assert(body.contains("state"));
    assert(body.contains("health"));
    if (device.part().kind == "gimbal") {
      assert(body.size() == 7);
      for (const char* field : {"pan_rad", "tilt_rad", "pan_rate_rad_s", "tilt_rate_rad_s"})
        assert(body.at(field).is_number());
    } else {
      assert(body.size() == 3);
    }
    const std::string encoded = body.dump();
    assert(encoded.find("pcm") == std::string::npos);
    assert(encoded.find("jpeg") == std::string::npos);
    assert(encoded.find("configuration") == std::string::npos);
  }

  // Semantic detections come from model output. No configured scene is
  // projected into a fabricated world resource.
  const nlohmann::json detection = projection.BuildDetection();
  assert(detection["type"] == "detection.snapshot");
  assert(detection["schema_version"] == "1.0");
  assert(detection["source"] ==
         registry.Find(hardware::DeviceKind::kCamera).identity().device_id);
  assert(detection["image_width_px"] == 1280);
  assert(detection["image_height_px"] == 720);
  assert(!detection["detections"].empty());
  assert(detection["detections"].front()["id"].get<std::string>().rfind("human:", 0) == 0);
  assert(detection["detections"].front()["bbox"] == nlohmann::json({0.0, 0.0, 100.0, 200.0}));
  assert(detection["detections"].front()["attributes"]["pose"] == "stand");
  assert(detection["detections"].front()["attributes"]["pose_confidence"] == 0.87);
  assert(!detection["detections"].front()["attributes"].contains("posture_state"));
  assert(detection["detections"].front()["attributes"]["action"] == "unknown");
  assert(!detection["detections"].front()["attributes"].contains("action_confidence"));
  assert(detection["detections"].front()["attributes"]["activity"] == "stand");
  const std::string encoded_detection = detection.dump();
  assert(encoded_detection.find("keypoint") == std::string::npos);
  // The generic URI router is the final core seam consumed by the gateway.
  const std::string root = "robots/" + registry.robot().name;
  bool world_rejected = false;
  try {
    projection.BuildByUri(root + "/world/state");
  } catch (const std::runtime_error&) {
    world_rejected = true;
  }
  assert(world_rejected);
  assert(projection.BuildByUri(root + "/detection/state").at("type") == "detection.snapshot");
  bool rejected_legacy_vision = false;
  try {
    projection.BuildByUri(root + "/world/vision/state");
  } catch (const std::runtime_error&) {
    rejected_legacy_vision = true;
  }
  assert(rejected_legacy_vision);

  runtime::models::MicrophoneVadStore vad_store;
  runtime::perception::body::Reader audio_projection(registry, clock, nullptr, nullptr, &vad_store);
  runtime::perception::ProductHistory vad_history(
      {root + "/world/vad/state", "egia.contracts.perception.world.vad.v1", "boot", "epoch",
       [](const nlohmann::json& value) {
         assert(!value.contains("pcm"));
         assert(!value.contains("wav"));
         assert(!value.contains("utterance"));
       }},
      4);
  auto vad_writer = vad_history.TakeWriter();
  vad_store.SetUpdatedCallback([&](bool) {
    vad_writer.Append(std::chrono::system_clock::now(), audio_projection.BuildVad());
  });
  runtime::perception::ResourceCatalog resources(&audio_projection);
  resources.Register(vad_history.Reader());
  assert(resources.ListResources().dump().find("/world/vad/state") != std::string::npos);

  runtime::models::MicrophoneVadStore::Snapshot vad;
  vad.model_id = "vad";
  vad.source_timestamp_ns = 42;
  vad.response = {{"speech", true}, {"probability", 0.8}, {"segments", {1, 2}}};
  vad_store.Set(std::move(vad));
  const nlohmann::json retained_vad = resources.ReadResource(root + "/world/vad/state");
  assert(retained_vad.at("type") == "vad.snapshot");
  assert(retained_vad.at("source_sequence") == 1);
  assert(retained_vad.at("source_timestamp_ns") == 42);
  assert(!retained_vad.contains("segments"));

  std::puts("demo perception chain test: OK");
  return 0;
}

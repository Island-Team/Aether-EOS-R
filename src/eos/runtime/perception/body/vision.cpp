#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "detail.h"
#include "detection_overlay.h"
#include "reader.h"
#include "src/eos/runtime/models/camera_object.h"
#include "src/eos/runtime/models/camera_pose.h"
#include "src/eos/runtime/hardware/registry.h"
#include "src/eos/runtime/store/furniture_region_store.h"

namespace eos::runtime::perception::body {
using namespace detail;
namespace {
constexpr double kMinimumHumanBoxConfidence = 0.40;

std::optional<std::string> StringField(const nlohmann::json& value, const char* key) {
  const auto field = value.find(key);
  if (field == value.end() || !field->is_string()) return std::nullopt;
  return field->get<std::string>();
}

void CopyConfidence(const nlohmann::json& person, const char* key, nlohmann::json* attributes) {
  const auto value = person.find(key);
  if (value == person.end() || !value->is_number()) return;
  const double confidence = value->get<double>();
  if (confidence >= 0.0 && confidence <= 1.0) (*attributes)[key] = confidence;
}

const char* StatusName(models::CameraPoseStore::Status status) {
  using Status = models::CameraPoseStore::Status;
  switch (status) {
    case Status::kNotConfigured:
      return "not_configured";
    case Status::kPending:
      return "pending";
    case Status::kComplete:
      return "complete";
    case Status::kUnavailable:
      return "unavailable";
  }
  return "unavailable";
}

nlohmann::json ModelSection(const models::CameraPoseStore::ModelState& state) {
  return {{"status", StatusName(state.status)},
          {"sequence", state.sequence},
          {"source_generation", state.source_generation},
          {"requested_generation", state.requested_generation}};
}

std::string ObjectTrack(const nlohmann::json& object, std::size_t index) {
  std::string stem = object.value("label", std::string("object"));
  for (char& character : stem) {
    const auto value = static_cast<unsigned char>(character);
    if (!std::isalnum(value) && character != '-' && character != '_') character = '-';
  }
  if (stem.empty()) stem = "object";
  return stem + "-" + std::to_string(index);
}

std::string PersonTrack(const nlohmann::json& person, std::size_t index) {
  std::string track = std::to_string(index);
  if (person.contains("track_id") && person["track_id"].is_string()) {
    track = person["track_id"].get<std::string>();
  } else if (person.contains("track_id") && person["track_id"].is_number_unsigned()) {
    track = std::to_string(person["track_id"].get<std::uint64_t>());
  } else if (person.contains("track_id") && person["track_id"].is_number_integer()) {
    track = std::to_string(person["track_id"].get<std::int64_t>());
  }
  if (track.rfind("human:", 0) == 0) track.erase(0, 6);
  return track;
}

struct FurnitureResult {
  double map_x_m;
  double map_y_m;
  const store::FurnitureRegion* region;
};

std::optional<FurnitureResult> LocateFurniture(
    const nlohmann::json& person, std::uint32_t image_width_px,
    const hardware::BaseTelemetry& base, const hardware::ManipulatorTelemetry& arm,
    const std::vector<store::FurnitureRegion>& regions) {
  if (regions.empty() || image_width_px == 0 || !base.valid || !base.fresh ||
      !base.pose_valid || !base.localization_valid || !arm.joint_valid[3] ||
      !std::isfinite(base.x_m) || !std::isfinite(base.y_m) || !std::isfinite(base.yaw_rad) ||
      !std::isfinite(arm.joint_angles_rad[3]))
    return std::nullopt;
  const auto bbox = person.find("bbox");
  const auto distance = person.find("distance_m");
  if (bbox == person.end() || !bbox->is_array() || bbox->size() != 4 ||
      distance == person.end() || !distance->is_number())
    return std::nullopt;
  for (const auto& coordinate : *bbox)
    if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>())) return std::nullopt;
  const double distance_m = distance->get<double>();
  if (!std::isfinite(distance_m) || distance_m < 0.0) return std::nullopt;
  const double center_x = (bbox->at(0).get<double>() + bbox->at(2).get<double>()) / 2.0;
  const double image_center_x = static_cast<double>(image_width_px) / 2.0;
  if (std::abs(center_x - image_center_x) >
      static_cast<double>(image_width_px) * 0.10)
    return std::nullopt;
  const double bearing = base.yaw_rad + arm.joint_angles_rad[3];
  const double map_x_m = base.x_m + distance_m * std::cos(bearing);
  const double map_y_m = base.y_m + distance_m * std::sin(bearing);
  const store::FurnitureRegion* matched = nullptr;
  for (const auto& region : regions) {
    if (region.Contains(map_x_m, map_y_m)) {
      matched = &region;
      break;
    }
  }
  return FurnitureResult{map_x_m, map_y_m, matched};
}

}  // namespace

nlohmann::json Reader::BuildDetection() {
  const std::string robot_id =
      registry_.robots().empty() ? "default" : registry_.robots().front().id;
  return BuildDetection(robot_id);
}

nlohmann::json Reader::BuildDetection(const std::string& robot_id) {
  const auto people = camera_pose_ ? camera_pose_->Latest() : std::nullopt;
  if (!people) {
    throw ResourceUnavailable("state projection: no detection input for robot '" + robot_id + "'");
  }
  nlohmann::json snapshot =
      StartEnvelope("detection.snapshot", "camera", "default", nullptr, "detection/" + robot_id);
  snapshot.erase("part_id");
  snapshot.erase("description");
  snapshot.erase("tags");
  snapshot.erase("frame_id");
  const hardware_runtime::Device* camera = registry_.FindDevice(people->device_id);
  if (camera == nullptr || camera->robot_id() != robot_id ||
      camera->identity().kind != DeviceKind::kCamera) {
    throw ResourceUnavailable("state projection: detection source is not bound to robot '" +
                              robot_id + "'");
  }
  snapshot["source"] = people->device_id;
  snapshot["source_sequence"] = people->sequence;
  snapshot["source_generation"] = people->generation;
  if (!people->frame_id.empty()) snapshot["source_frame_id"] = people->frame_id;
  if (people->source_timestamp_ns) {
    snapshot["source_timestamp_ns"] = *people->source_timestamp_ns;
    snapshot["source_clock"] = hardware::SourceClockName(people->source_clock);
    if (!people->source_clock_id.empty()) snapshot["source_clock_id"] = people->source_clock_id;
  }
  snapshot["depth_available"] = people->depth_available;
  if (people->capture_gimbal) {
    snapshot["source_gimbal"] = {{"pan_rad", people->capture_gimbal->pan_rad},
                                 {"tilt_rad", people->capture_gimbal->tilt_rad},
                                 {"pan_rate_rad_s", people->capture_gimbal->pan_rate_rad_s},
                                 {"tilt_rate_rad_s", people->capture_gimbal->tilt_rate_rad_s}};
    if (people->capture_gimbal_sampled_monotonic_ns)
      snapshot["source_gimbal"]["sampled_monotonic_ns"] =
          *people->capture_gimbal_sampled_monotonic_ns;
  }
  snapshot["models"] = {{"human", ModelSection(people->human)},
                        {"pose", ModelSection(people->pose)},
                        {"action", ModelSection(people->action)},
                        {"face", ModelSection(people->face)}};
  snapshot["face_recognition_status"] =
      people->face_response.has_value()
          ? "complete"
          : (people->face.status == models::CameraPoseStore::Status::kPending
                 ? "pending"
                 : (people->face_requested ? "unavailable" : "not_configured"));
  if (people->face_generation && people->face_age_ms) {
    snapshot["face_observation"] = {{"source_generation", *people->face_generation},
                                    {"age_ms", *people->face_age_ms}};
  }
  if (people->pose.source_generation > 0 && people->pose_age_ms) {
    snapshot["pose_observation"] = {{"source_generation", people->pose.source_generation},
                                    {"age_ms", *people->pose_age_ms}};
    if (people->pose_source_timestamp_ns) {
      snapshot["pose_observation"]["source_timestamp_ns"] = *people->pose_source_timestamp_ns;
      snapshot["pose_observation"]["source_clock"] =
          hardware::SourceClockName(people->pose_source_clock);
      if (!people->pose_source_clock_id.empty())
        snapshot["pose_observation"]["source_clock_id"] = people->pose_source_clock_id;
    }
  }
  if (people->image_width_px > 0 && people->image_height_px > 0) {
    snapshot["image_width_px"] = people->image_width_px;
    snapshot["image_height_px"] = people->image_height_px;
  }
  snapshot["detections"] = nlohmann::json::array();
  std::optional<hardware::BaseTelemetry> base;
  std::optional<hardware::ManipulatorTelemetry> arm;
  for (const auto& device : registry_.devices()) {
    if (device.robot_id() != robot_id) continue;
    if (!base && device.identity().kind == DeviceKind::kBase)
      base = device.feeds().ReadBaseTelemetry();
    if (!arm && device.identity().kind == DeviceKind::kManipulator)
      arm = device.feeds().ReadManipulatorTelemetry();
  }
  const auto* robot = registry_.FindRobot(robot_id);
  const std::vector<store::FurnitureRegion>* regions = nullptr;
  if (furniture_regions_ && robot) regions = &furniture_regions_->List(robot->name);
  const auto& persons = people->response.value("persons", nlohmann::json::array());
  std::size_t index = 0;
  for (const auto& person : persons) {
    const auto confidence = person.find("confidence");
    if (confidence == person.end() || !confidence->is_number() ||
        !std::isfinite(confidence->get<double>()) ||
        confidence->get<double>() < kMinimumHumanBoxConfidence)
      continue;
    const std::string track = PersonTrack(person, index++);
    nlohmann::json detection{{"id", "human:" + track},
                             {"kind", "human"},
                             {"label", "person"},
                             {"confidence", person.value("confidence", 0.0)}};
    if (person.contains("bbox") && person.at("bbox").is_array() && person.at("bbox").size() == 4) {
      detection["bbox"] = person.at("bbox");
    }
    if (person.contains("distance_m") && person["distance_m"].is_number())
      detection["distance_m"] = person["distance_m"];
    if (person.contains("bearing_rad") && person["bearing_rad"].is_number())
      detection["bearing_rad"] = person["bearing_rad"];
    if (person.contains("keypoints") && person["keypoints"].is_array() &&
        person["keypoints"].size() == 17)
      detection["keypoints"] = person["keypoints"];
    if (person.contains("keypoint_scores") && person["keypoint_scores"].is_array() &&
        person["keypoint_scores"].size() == 17)
      detection["keypoint_scores"] = person["keypoint_scores"];
    if (person.contains("keypoint_depth_m") && person["keypoint_depth_m"].is_array() &&
        person["keypoint_depth_m"].size() == 17)
      detection["keypoint_depth_m"] = person["keypoint_depth_m"];
    nlohmann::json attributes = nlohmann::json::object();
    const std::string raw_pose = person.value("pose", "unknown");
    const std::string pose = raw_pose == "stand" || raw_pose == "sit" || raw_pose == "lie"
                                 ? raw_pose : "unknown";
    const std::string raw_action = person.value("action", "unknown");
    std::string action = raw_action == "waving" ? "waving" : "unknown";
    if (action != "waving" && (pose == "sit" || pose == "lie") && base && arm && regions) {
      const auto furniture = LocateFurniture(person, people->image_width_px, *base, *arm, *regions);
      if (furniture) {
        const bool on_furniture = furniture->region != nullptr;
        action = on_furniture ? "rest" : pose == "lie" ? "fallen" : "unknown";
        attributes["on_furniture"] = on_furniture;
        attributes["map_x_m"] = furniture->map_x_m;
        attributes["map_y_m"] = furniture->map_y_m;
        attributes["position_source"] = "centered_distance_joint4";
        if (furniture->region) attributes["furniture_region_id"] = furniture->region->id;
      }
    }
    attributes["pose"] = pose;
    attributes["action"] = action;
    if (pose == raw_pose) CopyConfidence(person, "pose_confidence", &attributes);
    if (const auto source = StringField(person, "posture_source"))
      attributes["posture_source"] = *source;
    if (action == "rest" || action == "fallen") attributes["action_source"] = "furniture_map";
    else if (const auto source = StringField(person, "action_source"))
      attributes["action_source"] = *source;
    if (person.contains("posture_age_ms") && person["posture_age_ms"].is_number_integer())
      attributes["posture_age_ms"] = person["posture_age_ms"];
    if (action == raw_action) CopyConfidence(person, "action_confidence", &attributes);
    attributes["activity"] =
        action == "waving" || action == "fallen" || action == "rest" ? action : pose;
    if (person.contains("speaking") && person["speaking"].is_boolean())
      attributes["speaking"] = person["speaking"];
    attributes["name"] = nullptr;
    const auto identity = people->face_identities.find(track);
    if (identity != people->face_identities.end()) {
      attributes["name"] = identity->second.identity;
      attributes["name_source_generation"] = identity->second.source_generation;
      attributes["name_similarity"] = identity->second.similarity;
    }
    if (!attributes.empty()) detection["attributes"] = std::move(attributes);
    snapshot["detections"].push_back(std::move(detection));
  }
  const auto& furniture = people->response.value("furniture", nlohmann::json::array());
  index = 0;
  for (const auto& item : furniture) {
    if (!item.is_object() || !item.contains("class_name") || !item.at("class_name").is_string() ||
        !item.contains("bbox") || !item.at("bbox").is_array() || item.at("bbox").size() != 4)
      continue;
    const std::string label = item.at("class_name").get<std::string>();
    nlohmann::json detection{{"id", "obj:furniture-" + ObjectTrack({{"label", label}}, index++)},
                             {"kind", "object"},
                             {"label", label},
                             {"bbox", item.at("bbox")},
                             {"attributes", {{"furniture", true}}}};
    snapshot["detections"].push_back(std::move(detection));
  }
  if (detection_overlay_) detection_overlay_->Apply(&snapshot);
  return snapshot;
}

nlohmann::json Reader::BuildHeadDetection(const std::string& robot_id) {
  if (!default_object_camera_device_id_.empty())
    return BuildCameraObjectDetection(robot_id, default_object_camera_device_id_);
  for (const auto& device : registry_.devices()) {
    if (device.robot_id() == robot_id && device.identity().kind == DeviceKind::kCamera &&
        device.part().part_id == "head")
      return BuildCameraObjectDetection(robot_id, device.identity().device_id);
  }
  throw ResourceUnavailable("state projection: no default object camera for robot '" + robot_id +
                            "'");
}

nlohmann::json Reader::BuildCameraObjectDetection(const std::string& robot_id,
                                                   const std::string& camera_device_id) {
  const auto objects = camera_object_ ? camera_object_->Latest(camera_device_id) : std::nullopt;
  if (!objects) {
    throw ResourceUnavailable("state projection: no camera detection input for robot '" + robot_id +
                              "'");
  }
  return ProjectCameraObjectDetection(robot_id, *objects);
}

nlohmann::json Reader::ProjectCameraObjectDetection(
    const std::string& robot_id, const models::CameraObjectStore::Snapshot& object_snapshot) {
  const auto* objects = &object_snapshot;
  const hardware_runtime::Device* camera = registry_.FindDevice(objects->device_id);
  if (camera == nullptr || camera->robot_id() != robot_id ||
      camera->identity().kind != DeviceKind::kCamera) {
    throw ResourceUnavailable("state projection: camera detection source is not bound to robot '" +
                              robot_id + "'");
  }
  nlohmann::json snapshot = StartEnvelope("detection.snapshot", "camera", objects->part_id,
                                          &camera->part(), "detection/" + robot_id + "/head");
  snapshot.erase("part_id");
  snapshot.erase("description");
  snapshot.erase("tags");
  snapshot.erase("frame_id");
  snapshot["source"] = objects->device_id;
  snapshot["source_sequence"] = objects->sequence;
  snapshot["source_generation"] = objects->generation;
  if (!objects->frame_id.empty()) snapshot["source_frame_id"] = objects->frame_id;
  if (objects->source_timestamp_ns) {
    snapshot["source_timestamp_ns"] = *objects->source_timestamp_ns;
    snapshot["source_clock"] = hardware::SourceClockName(objects->source_clock);
    if (!objects->source_clock_id.empty()) snapshot["source_clock_id"] = objects->source_clock_id;
  }
  if (objects->image_width_px > 0 && objects->image_height_px > 0) {
    snapshot["image_width_px"] = objects->image_width_px;
    snapshot["image_height_px"] = objects->image_height_px;
  }
  snapshot["detections"] = nlohmann::json::array();
  const auto& items = objects->response.value("objects", nlohmann::json::array());
  std::size_t index = 0;
  for (const auto& object : items) {
    nlohmann::json detection{{"id", "obj:" + ObjectTrack(object, index++)},
                             {"kind", "object"},
                             {"label", object.at("label")},
                             {"confidence", object.at("confidence")}};
    if (object.contains("class_id")) detection["class_id"] = object.at("class_id");
    if (object.contains("source_label")) detection["source_label"] = object.at("source_label");
    if (object.contains("source_class_id"))
      detection["source_class_id"] = object.at("source_class_id");
    if (object.contains("bbox")) detection["bbox"] = object.at("bbox");
    if (object.contains("polygon")) detection["polygon"] = object.at("polygon");
    snapshot["detections"].push_back(std::move(detection));
  }
  return snapshot;
}

}  // namespace eos::runtime::perception::body

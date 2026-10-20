#include "detection_overlay.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace eos::runtime::perception::body {
namespace {

bool Box(const nlohmann::json& value) {
  if (!value.is_array() || value.size() != 4) return false;
  return std::all_of(value.begin(), value.end(), [](const auto& coordinate) {
    return coordinate.is_number() && std::isfinite(coordinate.template get<double>());
  }) && value[0].get<double>() < value[2].get<double>() &&
         value[1].get<double>() < value[3].get<double>();
}

double Overlap(const nlohmann::json& a, const nlohmann::json& b) {
  if (!Box(a) || !Box(b)) return 0.0;
  const double left = std::max(a[0].get<double>(), b[0].get<double>());
  const double top = std::max(a[1].get<double>(), b[1].get<double>());
  const double right = std::min(a[2].get<double>(), b[2].get<double>());
  const double bottom = std::min(a[3].get<double>(), b[3].get<double>());
  const double intersection = std::max(0.0, right - left) * std::max(0.0, bottom - top);
  const double area_a = (a[2].get<double>() - a[0].get<double>()) *
                        (a[3].get<double>() - a[1].get<double>());
  const double area_b = (b[2].get<double>() - b[0].get<double>()) *
                        (b[3].get<double>() - b[1].get<double>());
  return intersection / (area_a + area_b - intersection);
}

bool FiniteGimbal(const nlohmann::json& value) {
  if (!value.is_object()) return false;
  for (const char* key : {"pan_rad", "tilt_rad"}) {
    const auto field = value.find(key);
    if (field == value.end() || !field->is_number() ||
        !std::isfinite(field->get<double>())) return false;
  }
  return true;
}

bool SameGimbal(const nlohmann::json& overlay, const nlohmann::json& snapshot) {
  const auto expected = overlay.value("source_gimbal", nlohmann::json());
  const auto actual = snapshot.value("source_gimbal", nlohmann::json());
  if (!FiniteGimbal(expected) || !FiniteGimbal(actual)) return false;
  return std::abs(expected["pan_rad"].get<double>() - actual["pan_rad"].get<double>()) <= 0.05 &&
         std::abs(expected["tilt_rad"].get<double>() - actual["tilt_rad"].get<double>()) <= 0.05;
}

}  // namespace

void DetectionOverlayStore::Upsert(const std::string& execution_id, nlohmann::json overlay) {
  if (execution_id.empty() || !overlay.is_object() ||
      !overlay.value("source", nlohmann::json()).is_string() ||
      overlay["source"].get<std::string>().empty() ||
      !overlay.value("source_generation", nlohmann::json()).is_number_unsigned() ||
      !overlay.value("detection", nlohmann::json()).is_object() ||
      !Box(overlay.value("target_bbox", nlohmann::json())))
    throw std::invalid_argument("detection overlay requires execution, source, source_generation, target_bbox, and detection");
  auto& detection = overlay["detection"];
  if (detection.value("kind", nlohmann::json()) != "human" ||
      !Box(detection.value("bbox", nlohmann::json())) ||
      !detection.value("attributes", nlohmann::json()).is_object() ||
      detection["attributes"].value("pose", nlohmann::json()) != "lie" ||
      detection["attributes"].value("action", nlohmann::json()) != "fallen")
    throw std::invalid_argument("detection overlay requires a human lie/fallen correction");
  if (overlay.contains("target_id") && !overlay["target_id"].is_string())
    throw std::invalid_argument("detection overlay target_id must be a string");
  if (overlay.contains("source_gimbal") && !FiniteGimbal(overlay["source_gimbal"]))
    throw std::invalid_argument("detection overlay source_gimbal requires finite pan and tilt");
  if (overlay.contains("activation_generation") &&
      !overlay["activation_generation"].is_number_unsigned())
    throw std::invalid_argument("detection overlay activation_generation must be unsigned");
  if (detection.contains("id") &&
      (!detection["id"].is_string() ||
       detection["id"].get<std::string>().rfind("human:", 0) != 0))
    throw std::invalid_argument("detection overlay id must identify a human");
  std::lock_guard lock(mutex_);
  overlays_[execution_id] = std::move(overlay);
}

void DetectionOverlayStore::Clear(const std::string& execution_id) {
  std::lock_guard lock(mutex_);
  overlays_.erase(execution_id);
}

void DetectionOverlayStore::Apply(nlohmann::json* snapshot) const {
  if (!snapshot || !snapshot->is_object() || !snapshot->contains("detections") ||
      !(*snapshot)["detections"].is_array()) return;
  std::lock_guard lock(mutex_);
  for (const auto& [execution_id, overlay] : overlays_) {
    if (overlay["source"] != snapshot->value("source", nlohmann::json())) continue;
    const auto generation = snapshot->value("source_generation", nlohmann::json());
    if (!generation.is_number_unsigned() ||
        generation.get<std::uint64_t>() < overlay["source_generation"].get<std::uint64_t>())
      continue;
    auto& detections = (*snapshot)["detections"];
    auto selected = detections.end();
    for (auto it = detections.begin(); it != detections.end(); ++it) {
      if (!it->is_object() || it->value("kind", nlohmann::json()) != "human") continue;
      if (overlay.contains("target_id") && it->value("id", nlohmann::json()) == overlay["target_id"]) {
        selected = it;
        break;
      }
      if (Overlap(it->value("bbox", nlohmann::json()), overlay["target_bbox"]) >= 0.5)
        selected = it;
    }
    nlohmann::json correction = overlay["detection"];
    if (selected != detections.end()) {
      // Keep a live track ID and any ordinary fields that the correction does
      // not provide, while replacing the pose and aligned depth evidence.
      nlohmann::json merged = *selected;
      const bool exact_generation = snapshot->value("source_generation", nlohmann::json()) ==
                                    overlay["source_generation"];
      const bool activation_generation = overlay.contains("activation_generation") &&
          snapshot->value("source_generation", nlohmann::json()) == overlay["activation_generation"];
      const bool use_corrected_geometry = exact_generation ||
          (activation_generation && SameGimbal(overlay, *snapshot));
      for (auto field = correction.begin(); field != correction.end(); ++field)
        if (field.key() != "attributes" && field.key() != "id" &&
            (use_corrected_geometry || (field.key() != "bbox" && field.key() != "distance_m" &&
                                  field.key() != "bearing_rad" && field.key() != "keypoints" &&
                                  field.key() != "keypoint_scores" &&
                                  field.key() != "keypoint_depth_m")))
          merged[field.key()] = field.value();
      for (auto attribute = correction["attributes"].begin();
           attribute != correction["attributes"].end(); ++attribute) {
        const auto& key = attribute.key();
        if (use_corrected_geometry || key == "pose" || key == "action" || key == "activity" ||
            key == "name" || key == "name_similarity" || key == "name_source_generation")
          merged["attributes"][key] = attribute.value();
      }
      *selected = std::move(merged);
    } else if (snapshot->value("source_generation", nlohmann::json()) ==
               overlay["source_generation"]) {
      correction["id"] = correction.value("id", overlay.value(
          "target_id", std::string("human:observed-" + execution_id)));
      detections.push_back(std::move(correction));
    }
  }
}

}  // namespace eos::runtime::perception::body

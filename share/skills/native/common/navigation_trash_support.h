#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <nlohmann/json.hpp>

namespace island::skills::navigation_trash {

inline constexpr double kMinimumConfidence = 0.40;
inline constexpr std::size_t kRequiredConsecutiveFrames = 3;
inline constexpr std::size_t kRequiredAbsentFrames = 10;

struct Trash {
  const char* label;
  const char* uri;
};

inline constexpr std::array<Trash, 4> kTrash{{
    {"peel", "asset://head_find_trash/found_peel.wav"},
    {"empty_box", "asset://head_find_trash/found_empty_box.wav"},
    {"bottle", "asset://head_find_trash/found_bottle.wav"},
    {"paper_ball", "asset://head_find_trash/found_paper_ball.wav"},
}};

inline std::optional<std::uint64_t> SourceSequence(const nlohmann::json& snapshot) {
  const auto found = snapshot.find("source_sequence");
  if (found == snapshot.end() || !found->is_number_integer()) return std::nullopt;
  if (found->is_number_unsigned()) return found->get<std::uint64_t>();
  const auto value = found->get<std::int64_t>();
  return value < 0 ? std::nullopt : std::optional<std::uint64_t>(value);
}

inline bool SelectTrash(const nlohmann::json& snapshot,
                        std::array<bool, kTrash.size()>* present) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return false;
  present->fill(false);
  for (const auto& item : *detections) {
    if (!item.is_object() || item.value("kind", "") != "object" ||
        !item.contains("confidence") || !item.at("confidence").is_number())
      continue;
    const double confidence = item.at("confidence").get<double>();
    if (confidence < kMinimumConfidence || confidence > 1.0) continue;
    for (std::size_t index = 0; index < kTrash.size(); ++index) {
      if (item.value("label", "") == kTrash[index].label) (*present)[index] = true;
    }
  }
  return true;
}

class Detector {
 public:
  std::optional<std::size_t> Observe(const nlohmann::json& snapshot) {
    const auto sequence = SourceSequence(snapshot);
    if (!sequence || (last_sequence_ && *sequence <= *last_sequence_)) return std::nullopt;
    last_sequence_ = sequence;
    std::array<bool, kTrash.size()> present;
    if (!SelectTrash(snapshot, &present)) return std::nullopt;
    for (std::size_t index = 0; index < present.size(); ++index) {
      if (announced_[index]) {
        if (present[index]) absent_[index] = 0;
        else if (++absent_[index] >= kRequiredAbsentFrames) {
          announced_[index] = false;
          absent_[index] = 0;
          consecutive_[index] = 0;
        }
        continue;
      }
      if (!present[index]) {
        consecutive_[index] = 0;
      } else if (++consecutive_[index] >= kRequiredConsecutiveFrames) {
        announced_[index] = true;
        absent_[index] = 0;
        return index;
      }
    }
    return std::nullopt;
  }

 private:
  std::optional<std::uint64_t> last_sequence_;
  std::array<std::size_t, kTrash.size()> consecutive_{};
  std::array<std::size_t, kTrash.size()> absent_{};
  std::array<bool, kTrash.size()> announced_{};
};

inline std::optional<nlohmann::json> AbsoluteMapTarget(const nlohmann::json& status,
                                                       const nlohmann::json& relative) {
  if (!status.is_object() || !status.value("telemetry_valid", false) ||
      !status.value("fresh", false) || !status.value("pose_valid", false) ||
      !status.value("localization_valid", false) || status.value("frame_id", "") != "map" ||
      !status.contains("pose") || !status.at("pose").is_object())
    return std::nullopt;
  const auto& pose = status.at("pose");
  for (const char* key : {"x_m", "y_m", "yaw_rad"}) {
    if (!pose.contains(key) || !pose.at(key).is_number() ||
        !std::isfinite(pose.at(key).get<double>()) || !relative.contains(key) ||
        !relative.at(key).is_number() || !std::isfinite(relative.at(key).get<double>()))
      return std::nullopt;
  }
  const double yaw = pose.at("yaw_rad").get<double>();
  const double x = relative.at("x_m").get<double>();
  const double y = relative.at("y_m").get<double>();
  return nlohmann::json{
      {"reference", "absolute"},
      {"frame_id", "map"},
      {"x_m", pose.at("x_m").get<double>() + std::cos(yaw) * x - std::sin(yaw) * y},
      {"y_m", pose.at("y_m").get<double>() + std::sin(yaw) * x + std::cos(yaw) * y},
      {"yaw_rad", std::remainder(yaw + relative.at("yaw_rad").get<double>(),
                                  2.0 * std::acos(-1.0))}};
}

}  // namespace island::skills::navigation_trash

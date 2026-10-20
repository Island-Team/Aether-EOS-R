#include "src/eos/runtime/models/camera_pose.h"

#include <png.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "src/eos/lib/log.h"

namespace eos::runtime::models {
std::vector<std::uint8_t> EncodeAlignedDepthPng(const std::vector<std::uint16_t>& pixels,
                                                std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0 || pixels.size() != static_cast<std::size_t>(width) * height)
    return {};
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_LINEAR_Y;
  png_alloc_size_t size = 0;
  if (!png_image_write_to_memory(&image, nullptr, &size, 0, pixels.data(), 0, nullptr) || !size)
    return {};
  std::vector<std::uint8_t> result(size);
  if (!png_image_write_to_memory(&image, result.data(), &size, 0, pixels.data(), 0, nullptr))
    return {};
  result.resize(size);
  return result;
}

namespace {

constexpr auto kFaceObservationTtl = std::chrono::seconds(3);
constexpr auto kGemmaRetryInterval = std::chrono::seconds(10);
constexpr auto kGemmaChangeRetryInterval = std::chrono::seconds(1);
constexpr std::size_t kGemmaStableFrames = 8;
constexpr auto kGemmaStableDuration = std::chrono::milliseconds(250);
constexpr double kGemmaMinimumTrackConfidence = 0.7;
constexpr double kGemmaMinimumBoxIou = 0.8;
constexpr double kGemmaMinimumImageArea = 0.1;
constexpr double kMinimumFaceIdentitySimilarity = 0.40;
constexpr auto kFaceUnresolvedRequestInterval = std::chrono::milliseconds(250);
constexpr auto kFaceRecognizedRefreshInterval = std::chrono::milliseconds(1000);
constexpr auto kFaceRequestMaximumAge = std::chrono::seconds(3);
constexpr auto kFaceMetricsWindow = std::chrono::seconds(30);

bool HasFaceCandidate(const CameraPoseStore::Snapshot& snapshot) {
  if (snapshot.human.status != CameraPoseStore::Status::kComplete) return false;
  const auto persons = snapshot.response.find("persons");
  if (persons == snapshot.response.end() || !persons->is_array()) return false;
  return std::any_of(persons->begin(), persons->end(), [](const auto& person) {
    return person.is_object() && person.contains("track_id") && person.contains("bbox");
  });
}

bool HasKnownFaceIdentity(const CameraPoseStore::Snapshot& snapshot) {
  return std::any_of(snapshot.face_identities.begin(), snapshot.face_identities.end(),
                     [](const auto& entry) {
                       return !entry.second.identity.empty() && entry.second.identity != "unknown";
                     });
}

std::string_view CallStatusName(CallResult::Status status) {
  switch (status) {
    case CallResult::Status::kCompleted:
      return "completed";
    case CallResult::Status::kInvalidArgument:
      return "invalid_argument";
    case CallResult::Status::kNotFound:
      return "not_found";
    case CallResult::Status::kUnavailable:
      return "unavailable";
    case CallResult::Status::kDeadlineExceeded:
      return "deadline_exceeded";
    case CallResult::Status::kRemoteError:
      return "remote_error";
    case CallResult::Status::kProtocolError:
      return "protocol_error";
    case CallResult::Status::kInternal:
      return "internal";
  }
  return "unknown";
}

CameraPoseStore::Snapshot MakeSourceSnapshot(const hardware::CameraTelemetry& frame,
                                             std::uint64_t generation, std::string model_id,
                                             std::string device_id) {
  return {.model_id = std::move(model_id),
          .device_id = std::move(device_id),
          .generation = generation,
          .frame_id = frame.frame_id,
          .source_timestamp_ns = frame.source_timestamp_ns,
          .source_clock = frame.source_clock,
          .source_clock_id = frame.source_clock_id,
          .depth_available = !frame.depth_png.empty() || !frame.depth_u16.empty(),
          .image_width_px = frame.width_px,
          .image_height_px = frame.height_px,
          .jpeg = std::make_shared<const std::vector<std::uint8_t>>(frame.jpeg),
          .depth_png = std::make_shared<const std::vector<std::uint8_t>>(frame.depth_png),
          .depth_u16 = std::make_shared<const std::vector<std::uint16_t>>(frame.depth_u16),
          .depth_scale = frame.depth_scale,
          .depth_intrinsics = frame.depth_intrinsics,
          .capture_gimbal = frame.capture_gimbal,
          .capture_gimbal_sampled_monotonic_ns = frame.capture_gimbal_sampled_monotonic_ns,
          .opaque_context = frame.opaque_context};
}

std::optional<std::string> ExplicitTrack(const nlohmann::json& person) {
  const auto track = person.find("track_id");
  if (track == person.end()) return std::nullopt;
  if (track->is_string() && !track->get_ref<const std::string&>().empty()) {
    std::string value = track->get<std::string>();
    if (value.rfind("human:", 0) == 0) value.erase(0, 6);
    return value.empty() ? std::nullopt : std::optional<std::string>(std::move(value));
  }
  if (track->is_number_unsigned()) return std::to_string(track->get<std::uint64_t>());
  if (track->is_number_integer()) return std::to_string(track->get<std::int64_t>());
  return std::nullopt;
}

bool ReadBbox(const nlohmann::json& value, double* bbox) {
  if (!value.is_array() || value.size() != 4) return false;
  for (std::size_t index = 0; index < 4; ++index) {
    if (!value.at(index).is_number()) return false;
    bbox[index] = value.at(index).get<double>();
    if (!std::isfinite(bbox[index])) return false;
  }
  return bbox[0] < bbox[2] && bbox[1] < bbox[3];
}

double BoxIou(const nlohmann::json& first, const nlohmann::json& second) {
  double a[4], b[4];
  if (!ReadBbox(first, a) || !ReadBbox(second, b)) return 0.0;
  const double intersection = std::max(0.0, std::min(a[2], b[2]) - std::max(a[0], b[0])) *
                              std::max(0.0, std::min(a[3], b[3]) - std::max(a[1], b[1]));
  const double area_a = (a[2] - a[0]) * (a[3] - a[1]);
  const double area_b = (b[2] - b[0]) * (b[3] - b[1]);
  return intersection / (area_a + area_b - intersection);
}

std::map<std::string, std::size_t> TrackCounts(const nlohmann::json& persons) {
  std::map<std::string, std::size_t> counts;
  if (!persons.is_array()) return counts;
  for (const auto& person : persons) {
    if (!person.is_object()) continue;
    const auto track = ExplicitTrack(person);
    if (track) ++counts[*track];
  }
  return counts;
}

void ClearFaceObservation(CameraPoseStore::Snapshot& snapshot) {
  snapshot.face_response.reset();
  snapshot.face_pose_response.reset();
  snapshot.face_generation.reset();
  snapshot.face_completed_at.reset();
  snapshot.face_age_ms.reset();
}

std::int64_t MonotonicNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Base64Encode(const std::vector<std::uint8_t>& input) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve((input.size() + 2) / 3 * 4);
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const std::uint32_t a = input[offset];
    const std::uint32_t b = offset + 1 < input.size() ? input[offset + 1] : 0;
    const std::uint32_t c = offset + 2 < input.size() ? input[offset + 2] : 0;
    const std::uint32_t value = (a << 16) | (b << 8) | c;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(offset + 1 < input.size() ? kAlphabet[(value >> 6) & 0x3f] : '=');
    output.push_back(offset + 2 < input.size() ? kAlphabet[value & 0x3f] : '=');
  }
  return output;
}

void RetainPersonEnrichment(const nlohmann::json& previous, nlohmann::json& current) {
  if (!previous.contains("persons") || !previous.at("persons").is_array() ||
      !current.contains("persons") || !current.at("persons").is_array()) {
    return;
  }
  for (auto& person : current["persons"]) {
    if (!person.is_object() || !person.contains("track_id")) continue;
    const auto retained = std::find_if(
        previous.at("persons").begin(), previous.at("persons").end(), [&](const auto& candidate) {
          return candidate.is_object() && candidate.contains("track_id") &&
                 candidate.at("track_id") == person.at("track_id");
        });
    if (retained == previous.at("persons").end()) continue;
    for (const auto& [key, value] : retained->items()) {
      if (key == "posture_source" || key == "posture_age_ms" ||
          (key == "posture_state" && retained->value("posture_source", "") == "gemma4")) continue;
      if (key != "track_id" && key != "bbox" && key != "confidence" && key != "distance_m" &&
          key != "posture_source" && key != "posture_age_ms" && key != "action_source" &&
          !person.contains(key))
        person[key] = value;
    }
  }
}

void AddDepthDistances(nlohmann::json& response, const hardware::CameraTelemetry& frame) {
  if (frame.depth_u16.size() != static_cast<std::size_t>(frame.width_px) * frame.height_px ||
      !response.contains("persons") || !response.at("persons").is_array())
    return;
  for (auto& person : response["persons"]) {
    if (!person.is_object() || !person.contains("bbox") || !person.at("bbox").is_array() ||
        person.at("bbox").size() != 4)
      continue;
    const auto& bbox = person.at("bbox");
    const double x1 =
        std::clamp(bbox.at(0).get<double>(), 0.0, static_cast<double>(frame.width_px));
    const double y1 =
        std::clamp(bbox.at(1).get<double>(), 0.0, static_cast<double>(frame.height_px));
    const double x2 =
        std::clamp(bbox.at(2).get<double>(), 0.0, static_cast<double>(frame.width_px));
    const double y2 =
        std::clamp(bbox.at(3).get<double>(), 0.0, static_cast<double>(frame.height_px));
    const double roi_w = std::max(6.0, (x2 - x1) * 0.35);
    const double roi_h = std::max(6.0, (y2 - y1) * 0.35);
    const double cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5;
    const int left = std::max(0, static_cast<int>(std::floor(cx - roi_w * 0.5)));
    const int right =
        std::min(static_cast<int>(frame.width_px), static_cast<int>(std::ceil(cx + roi_w * 0.5)));
    const int top = std::max(0, static_cast<int>(std::floor(cy - roi_h * 0.5)));
    const int bottom =
        std::min(static_cast<int>(frame.height_px), static_cast<int>(std::ceil(cy + roi_h * 0.5)));
    const int sx = std::max(1, (right - left + 23) / 24);
    const int sy = std::max(1, (bottom - top + 23) / 24);
    std::vector<std::uint16_t> samples;
    for (int y = top; y < bottom; y += sy)
      for (int x = left; x < right; x += sx) {
        const auto value = frame.depth_u16[static_cast<std::size_t>(y) * frame.width_px + x];
        if (value) samples.push_back(value);
      }
    if (samples.empty()) continue;
    const auto middle = samples.begin() + samples.size() / 2;
    std::nth_element(samples.begin(), middle, samples.end());
    double median = *middle;
    if (samples.size() % 2 == 0) {
      median = (median + *std::max_element(samples.begin(), middle)) * 0.5;
    }
    person["distance_m"] = median * frame.depth_scale;
  }
}

void AddKeypointDepthsInternal(nlohmann::json& response, const hardware::CameraTelemetry& frame) {
  constexpr double kMinimumKeypointConfidence = 0.2;
  constexpr int kRadius = 2;
  if (frame.depth_u16.size() != static_cast<std::size_t>(frame.width_px) * frame.height_px ||
      !response.contains("persons") || !response.at("persons").is_array())
    return;
  for (auto& person : response["persons"]) {
    if (!person.is_object() || !person.contains("keypoints") ||
        !person.at("keypoints").is_array() || person.at("keypoints").size() != 17 ||
        !person.contains("keypoint_scores") || !person.at("keypoint_scores").is_array() ||
        person.at("keypoint_scores").size() != 17)
      continue;
    nlohmann::json depths = nlohmann::json::array();
    for (std::size_t index = 0; index < 17; ++index) {
      const auto& point = person.at("keypoints").at(index);
      const auto& score = person.at("keypoint_scores").at(index);
      if (!point.is_array() || point.size() != 2 || !point.at(0).is_number() ||
          !point.at(1).is_number() || !score.is_number() ||
          score.get<double>() < kMinimumKeypointConfidence) {
        depths.push_back(nullptr);
        continue;
      }
      const double px = point.at(0).get<double>();
      const double py = point.at(1).get<double>();
      if (!std::isfinite(px) || !std::isfinite(py) || px < 0 || py < 0 || px >= frame.width_px ||
          py >= frame.height_px) {
        depths.push_back(nullptr);
        continue;
      }
      const int cx = static_cast<int>(std::lround(px));
      const int cy = static_cast<int>(std::lround(py));
      std::vector<std::uint16_t> samples;
      samples.reserve((kRadius * 2 + 1) * (kRadius * 2 + 1));
      for (int y = std::max(0, cy - kRadius);
           y <= std::min(static_cast<int>(frame.height_px) - 1, cy + kRadius); ++y) {
        for (int x = std::max(0, cx - kRadius);
             x <= std::min(static_cast<int>(frame.width_px) - 1, cx + kRadius); ++x) {
          const auto raw = frame.depth_u16[static_cast<std::size_t>(y) * frame.width_px + x];
          if (raw != 0) samples.push_back(raw);
        }
      }
      if (samples.size() < 3) {
        depths.push_back(nullptr);
        continue;
      }
      const auto middle = samples.begin() + samples.size() / 2;
      std::nth_element(samples.begin(), middle, samples.end());
      double median = *middle;
      if (samples.size() % 2 == 0)
        median = (median + *std::max_element(samples.begin(), middle)) * 0.5;
      depths.push_back(median * frame.depth_scale);
    }
    person["keypoint_depth_m"] = std::move(depths);
  }
}

}  // namespace

void AddAlignedKeypointDepths(nlohmann::json& response, const hardware::CameraTelemetry& frame) {
  AddKeypointDepthsInternal(response, frame);
}

void CameraPoseStore::Set(Snapshot snapshot) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.face_response) UpdateFaceIdentityCache(snapshot, *snapshot.face_response);
    Publish(std::move(snapshot));
    updated = updated_;
  }
  if (updated) updated();
}

void CameraPoseStore::SetSource(Snapshot snapshot, bool pose_enabled, bool pose_requested,
                                bool face_enabled, bool face_requested) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.response =
        latest_ ? latest_->response
                : nlohmann::json{{"persons", nlohmann::json::array()}, {"person_count", 0}};
    snapshot.face_requested = face_requested;
    snapshot.human = {Status::kPending, human_sequence_,
                      latest_ ? latest_->human.source_generation : 0, snapshot.generation};
    const auto retained_state = [&](ModelState previous, bool configured, bool requested) {
      if (!configured) return ModelState{};
      if (previous.status == Status::kNotConfigured) previous.status = Status::kUnavailable;
      if (requested) {
        previous.status = Status::kPending;
        previous.requested_generation = snapshot.generation;
      }
      return previous;
    };
    snapshot.pose =
        retained_state(latest_ ? latest_->pose : ModelState{}, pose_enabled, pose_requested);
    snapshot.action =
        retained_state(latest_ ? latest_->action : ModelState{}, pose_enabled, pose_requested);
    snapshot.face =
        retained_state(latest_ ? latest_->face : ModelState{}, face_enabled, face_requested);
    if (latest_ && latest_->face_response) {
      snapshot.face_response = latest_->face_response;
      snapshot.face_pose_response = latest_->face_pose_response;
      snapshot.face_generation = latest_->face_generation;
      snapshot.face_completed_at = latest_->face_completed_at;
      snapshot.face_age_ms = latest_->face_age_ms;
    }
    if (latest_ && latest_->pose_observed_at) {
      snapshot.pose_source_timestamp_ns = latest_->pose_source_timestamp_ns;
      snapshot.pose_source_clock = latest_->pose_source_clock;
      snapshot.pose_source_clock_id = latest_->pose_source_clock_id;
      snapshot.pose_observed_at = latest_->pose_observed_at;
      snapshot.pose_age_ms = latest_->pose_age_ms;
    }
    for (auto early = early_face_results_.begin(); early != early_face_results_.end();) {
      if (early->first >= snapshot.generation) {
        ++early;
        continue;
      }
      const auto source = source_history_.find(early->first);
      if (source != source_history_.end()) {
        UpdateFaceIdentityCache(source->second, early->second.response);
        ApplyFaceObservation(snapshot, source->second, early->second.response,
                             early->second.completed_at);
      }
      early = early_face_results_.erase(early);
    }
    ResolveFaceIdentities(snapshot);
    source_history_.insert_or_assign(snapshot.generation, snapshot);
    while (source_history_.size() > 64) source_history_.erase(source_history_.begin());
    Publish(std::move(snapshot));
    updated = updated_;
  }
  if (updated) updated();
}

bool CameraPoseStore::SetHuman(std::uint64_t generation, nlohmann::json response) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || latest_->generation != generation || !response.contains("persons") ||
        !response.at("persons").is_array())
      return false;
    ObserveTrackInputs(response.at("persons"));
    RetainPersonEnrichment(latest_->response, response);
    std::set<std::string> current_tracks;
    for (const auto& person : response.at("persons")) {
      if (!person.is_object()) continue;
      if (const auto track = ExplicitTrack(person)) {
        current_tracks.insert(*track);
        track_first_generation_.try_emplace(*track, generation);
      }
    }
    for (auto it = track_first_generation_.begin(); it != track_first_generation_.end();) {
      if (current_tracks.find(it->first) == current_tracks.end()) {
        track_input_signatures_.erase(it->first);
        track_input_revisions_.erase(it->first);
        it = track_first_generation_.erase(it);
      } else ++it;
    }
    ApplyGemmaPosture(&response);
    latest_->response = std::move(response);
    latest_->human = {Status::kComplete, ++human_sequence_, generation, generation};
    source_history_.insert_or_assign(generation, *latest_);
    while (source_history_.size() > 64) source_history_.erase(source_history_.begin());
    auto pending = *latest_;
    pending.depth_u16.reset();
    pending_.insert_or_assign(generation, std::move(pending));
    while (pending_.size() > 64) pending_.erase(pending_.begin());
    const auto early_face = early_face_results_.find(generation);
    if (early_face != early_face_results_.end()) {
      UpdateFaceIdentityCache(*latest_, early_face->second.response);
      ApplyFaceObservation(*latest_, *latest_, early_face->second.response,
                           early_face->second.completed_at);
      pending_.insert_or_assign(generation, *latest_);
      early_face_results_.erase(early_face);
    }
    ResolveFaceIdentities(*latest_);
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

bool CameraPoseStore::SetUnavailable(std::uint64_t generation, ModelState Snapshot::* state,
                                     std::uint64_t* sequence) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || latest_->generation != generation) return false;
    (*latest_).*state = {Status::kUnavailable, ++*sequence, generation, generation};
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

bool CameraPoseStore::SetHumanUnavailable(std::uint64_t generation) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || latest_->generation != generation) return false;
    latest_->human = {Status::kUnavailable, ++human_sequence_, generation, generation};
    if (latest_->pose.status == Status::kPending)
      latest_->pose = {Status::kUnavailable, ++pose_sequence_, generation, generation};
    if (latest_->action.status == Status::kPending)
      latest_->action = {Status::kUnavailable, ++action_sequence_, generation, generation};
    const bool had_early_face = early_face_results_.erase(generation) > 0;
    if (latest_->face.status == Status::kPending || had_early_face)
      latest_->face = {Status::kUnavailable, ++face_sequence_, generation, generation};
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

bool CameraPoseStore::SetPoseUnavailable(std::uint64_t generation) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || latest_->generation != generation) return false;
    latest_->pose = {Status::kUnavailable, ++pose_sequence_, generation, generation};
    latest_->action = {Status::kUnavailable, ++action_sequence_, generation, generation};
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

void CameraPoseStore::SetPose(Snapshot snapshot, bool wait_for_face) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.face_response) UpdateFaceIdentityCache(snapshot, *snapshot.face_response);
    if (latest_ && latest_->face_response) {
      snapshot.face_response = latest_->face_response;
      snapshot.face_pose_response = latest_->face_pose_response;
      snapshot.face_generation = latest_->face_generation;
      snapshot.face_completed_at = latest_->face_completed_at;
      snapshot.face_age_ms = latest_->face_age_ms;
    }
    const std::uint64_t generation = snapshot.generation;
    Publish(std::move(snapshot));
    if (wait_for_face) {
      pending_.insert_or_assign(generation, *latest_);
      while (pending_.size() > 64) pending_.erase(pending_.begin());
    }
    updated = updated_;
  }
  if (updated) updated();
}

void CameraPoseStore::SetDetection(Snapshot snapshot, bool wait_for_face) {
  SetPose(std::move(snapshot), wait_for_face);
}

bool CameraPoseStore::MergePose(std::uint64_t generation, nlohmann::json response,
                                std::optional<std::int64_t> source_timestamp_ns,
                                hardware::SourceClockKind source_clock, std::string source_clock_id,
                                std::optional<std::chrono::steady_clock::time_point> observed_at) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || generation > latest_->generation || !response.contains("persons") ||
        !response.at("persons").is_array())
      return false;
    ObserveTrackInputs(response.at("persons"));
    auto& detected = latest_->response["persons"];
    if (!detected.is_array()) return false;
    bool merged = detected.empty() && response.at("persons").empty();
    for (const auto& posed : response.at("persons")) {
      if (!posed.is_object() || !posed.contains("track_id")) continue;
      const auto posed_track = ExplicitTrack(posed);
      if (!posed_track) continue;
      const auto found = std::find_if(detected.begin(), detected.end(), [&](const auto& person) {
        return person.is_object() && person.contains("track_id") &&
               person.at("track_id") == posed.at("track_id");
      });
      if (found == detected.end()) continue;
      const auto first_generation = track_first_generation_.find(*posed_track);
      if (first_generation != track_first_generation_.end() &&
          first_generation->second > generation)
        continue;
      for (const auto& [key, value] : posed.items()) {
        if (key != "track_id" && key != "bbox" && key != "confidence" && key != "distance_m")
          (*found)[key] = value;
      }
      merged = true;
    }
    if (!merged) return false;
    latest_->response["person_count"] = detected.size();
    ApplyGemmaPosture(&latest_->response);
    const auto pose_status =
        latest_->pose.requested_generation > generation ? Status::kPending : Status::kComplete;
    latest_->pose = {pose_status, ++pose_sequence_, generation, latest_->pose.requested_generation};
    latest_->action = {pose_status, ++action_sequence_, generation,
                       latest_->action.requested_generation};
    latest_->pose_source_timestamp_ns = source_timestamp_ns;
    latest_->pose_source_clock = source_clock;
    latest_->pose_source_clock_id = std::move(source_clock_id);
    latest_->pose_observed_at = observed_at.value_or(std::chrono::steady_clock::now());
    latest_->pose_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - *latest_->pose_observed_at)
                               .count();
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

void CameraPoseStore::ObserveTrackInputs(const nlohmann::json& persons) {
  if (!persons.is_array()) return;
  for (const auto& person : persons) {
    if (!person.is_object()) continue;
    const auto track = ExplicitTrack(person);
    if (!track || !person.contains("pose") || !person.at("pose").is_string()) continue;
    const std::string signature = person.at("pose").get<std::string>() + ":" +
        person.value("posture_state", "") + ":" +
        ((person.value("action", "") == "waving" ||
          person.value("behavior_event", "") == "waving_event") ? "waving" : "other");
    auto [it, inserted] = track_input_signatures_.try_emplace(*track, signature);
    if (inserted) track_input_revisions_[*track] = 1;
    if (!inserted && it->second != signature) {
      it->second = signature;
      ++track_input_revisions_[*track];
      gemma_observations_.erase(*track);
    }
  }
}

void CameraPoseStore::ApplyGemmaPosture(nlohmann::json* response) {
  if (!response || !response->contains("persons") || !response->at("persons").is_array()) return;
  const auto now = std::chrono::steady_clock::now();
  const auto& persons = response->at("persons");
  for (auto it = gemma_observations_.begin(); it != gemma_observations_.end();) {
    const bool present = std::any_of(persons.begin(), persons.end(), [&](const auto& person) {
      return person.is_object() && ExplicitTrack(person) == it->first;
    });
    if (!present) it = gemma_observations_.erase(it);
    else ++it;
  }
  for (auto& person : (*response)["persons"]) {
    if (!person.is_object()) continue;
    const std::string pose = person.value("pose", "unknown");
    const std::string yolo_action = person.value("action", "unknown");
    const bool yolo_waving = yolo_action == "waving";
    const auto track = ExplicitTrack(person);
    const auto observation = track ? gemma_observations_.find(*track) : gemma_observations_.end();
    const std::string yolo_state = person.value("posture_state", "");
    bool furniture = yolo_state == "sitting_on_furniture" ||
                     yolo_state == "lying_on_furniture";
    bool compatible = false;
    if (observation != gemma_observations_.end()) {
      const auto& state = observation->second.posture_state;
      compatible = (pose == "stand" && state == "standing") ||
                   (pose == "sit" && (state == "sitting" || state == "sitting_on_furniture")) ||
                   (pose == "lie" && (state == "lying" || state == "lying_on_furniture" ||
                                       state == "lying_on_floor"));
      if (compatible) {
        person["posture_state"] = state;
        person["posture_source"] = "gemma4";
        person["posture_age_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - observation->second.completed_at).count();
        furniture = state == "sitting_on_furniture" || state == "lying_on_furniture";
      }
    }
    if (!compatible) {
      if (yolo_state.empty()) person.erase("posture_state");
      person["posture_source"] = "yolo";
      person["posture_age_ms"] = 0;
    }
    person.erase("posture");
    person.erase("posture_confidence");
    person.erase("behavior_confidence");
    const std::string action = yolo_waving && pose != "lie" ? "waving" : "unknown";
    person["action"] = action;
    person["behavior_event"] = action == "fallen" ? "fallen_event" :
                               action == "waving" ? "waving_event" : action;
    if (action != "waving") person.erase("action_confidence");
    person["action_source"] = "yolo";
  }
}

bool CameraPoseStore::SetGemmaPosture(std::uint64_t generation, std::string track_id,
                                      std::string posture_state, std::string action,
                                      std::uint64_t input_revision) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!latest_ || generation > latest_->generation ||
        !latest_->response.contains("persons") ||
        !latest_->response.at("persons").is_array()) return false;
    const auto& persons = latest_->response.at("persons");
    const auto found = std::find_if(persons.begin(), persons.end(), [&](const auto& person) {
      return person.is_object() && ExplicitTrack(person) == track_id;
    });
    if (found == persons.end()) return false;
    const auto first_generation = track_first_generation_.find(track_id);
    if (first_generation == track_first_generation_.end() ||
        first_generation->second > generation) return false;
    if (input_revision != 0 && track_input_revisions_[track_id] != input_revision) return false;
    gemma_observations_.insert_or_assign(
        track_id, GemmaObservation{track_id, std::move(posture_state),
                                   std::move(action), std::chrono::steady_clock::now()});
    ApplyGemmaPosture(&latest_->response);
    UpdateRecent();
    updated = updated_;
  }
  if (updated) updated();
  return true;
}

std::uint64_t CameraPoseStore::TrackInputRevision(std::string_view track_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = track_input_revisions_.find(std::string(track_id));
  return it == track_input_revisions_.end() ? 0 : it->second;
}

bool CameraPoseStore::RepublishLatest() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!latest_) return false;
  auto snapshot = *latest_;
  Publish(std::move(snapshot));
  return true;
}

std::uint64_t CameraPoseStore::NextGeneration() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("camera generation exhausted");
  }
  return ++generation_;
}

void CameraPoseStore::SetFace(std::uint64_t generation, nlohmann::json response) {
  std::lock_guard<std::mutex> update_lock(update_mutex_);
  Updated updated;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = pending_.find(generation);
    const auto completed_at = std::chrono::steady_clock::now();
    if (found == pending_.end()) {
      if (latest_ && latest_->generation == generation) {
        early_face_results_.insert_or_assign(
            generation, FaceResult{.response = std::move(response), .completed_at = completed_at});
        latest_->face = {Status::kComplete, ++face_sequence_, generation,
                         latest_->face.requested_generation};
        UpdateRecent();
        updated = updated_;
      } else if (const auto source = source_history_.find(generation);
                 source != source_history_.end()) {
        UpdateFaceIdentityCache(source->second, response);
        if (latest_) {
          ApplyFaceObservation(*latest_, source->second, response, completed_at);
          ResolveFaceIdentities(*latest_);
          if (latest_->generation == generation)
            latest_->face = {Status::kComplete, ++face_sequence_, generation,
                             latest_->face.requested_generation};
        }
        UpdateRecent();
        updated = updated_;
      } else {
        early_face_results_.insert_or_assign(
            generation, FaceResult{.response = std::move(response), .completed_at = completed_at});
        while (early_face_results_.size() > 64)
          early_face_results_.erase(early_face_results_.begin());
      }
    } else {
      UpdateFaceIdentityCache(found->second, response);
      if (latest_) ApplyFaceObservation(*latest_, found->second, response, completed_at);
      if (latest_) ResolveFaceIdentities(*latest_);
      if (latest_) {
        const auto status =
            latest_->face.requested_generation > generation ? Status::kPending : Status::kComplete;
        latest_->face = {status, ++face_sequence_, generation, latest_->face.requested_generation};
      }
      if (latest_) {
        const auto recent = std::find_if(
            recent_.begin(), recent_.end(),
            [&](const Snapshot& snapshot) { return snapshot.sequence == latest_->sequence; });
        if (recent != recent_.end())
          ApplyFaceObservation(*recent, found->second, response, completed_at);
      }
      pending_.erase(pending_.begin(), std::next(found));
      updated = updated_;
    }
  }
  if (updated) updated();
}

void CameraPoseStore::SetFaceUnavailable(std::uint64_t generation) {
  SetUnavailable(generation, &Snapshot::face, &face_sequence_);
  std::lock_guard<std::mutex> lock(mutex_);
  early_face_results_.erase(generation);
  const auto found = pending_.find(generation);
  if (found != pending_.end()) pending_.erase(pending_.begin(), std::next(found));
}

void CameraPoseStore::ApplyFaceObservation(Snapshot& snapshot, const Snapshot& source,
                                           const nlohmann::json& response,
                                           std::chrono::steady_clock::time_point completed_at) {
  snapshot.face_response = response;
  snapshot.face_pose_response = source.response;
  snapshot.face_generation = source.generation;
  snapshot.face_completed_at = completed_at;
  snapshot.face_age_ms = 0;
}

void CameraPoseStore::UpdateFaceIdentityCache(const Snapshot& source,
                                              const nlohmann::json& response) {
  const auto persons = source.response.find("persons");
  const auto data = response.find("data");
  if (persons == source.response.end() || !persons->is_array() || data == response.end() ||
      !data->is_object())
    return;
  const auto faces = data->find("faces");
  if (faces == data->end() || !faces->is_array()) return;
  const auto counts = TrackCounts(*persons);
  for (const auto& face : *faces) {
    if (!face.is_object()) continue;
    const auto identity = face.find("identity");
    const auto similarity = face.find("similarity");
    const auto face_bbox = face.find("bbox");
    if (face_bbox == face.end()) continue;
    double parsed_face[4]{};
    if (!ReadBbox(*face_bbox, parsed_face)) continue;
    const double center_x = (parsed_face[0] + parsed_face[2]) * 0.5;
    const double center_y = (parsed_face[1] + parsed_face[3]) * 0.5;
    const nlohmann::json* matched = nullptr;
    std::string matched_track;
    for (const auto& person : *persons) {
      if (!person.is_object() || !person.contains("bbox")) continue;
      const auto track = ExplicitTrack(person);
      double person_bbox[4]{};
      if (!track || counts.at(*track) != 1 || !ReadBbox(person.at("bbox"), person_bbox) ||
          center_x < person_bbox[0] || center_x > person_bbox[2] || center_y < person_bbox[1] ||
          center_y > person_bbox[3])
        continue;
      if (matched != nullptr) {
        matched = nullptr;
        matched_track.clear();
        break;
      }
      matched = &person;
      matched_track = *track;
    }
    if (matched == nullptr) continue;
    const auto existing = face_identity_cache_.find(matched_track);
    const double score =
        similarity != face.end() && similarity->is_number() ? similarity->get<double>() : 0.0;
    const bool accepted =
        face.value("known", false) && identity != face.end() && identity->is_string() &&
        !identity->get_ref<const std::string&>().empty() && std::isfinite(score) &&
        score >= kMinimumFaceIdentitySimilarity && score <= 1.0;
    if (!accepted) {
      if (existing == face_identity_cache_.end())
        face_identity_cache_.insert_or_assign(
            matched_track, CachedFaceIdentity{"unknown", 0.0, source.generation});
      continue;
    }
    if (existing != face_identity_cache_.end() &&
        existing->second.source_generation > source.generation)
      continue;
    face_identity_cache_.insert_or_assign(
        matched_track, CachedFaceIdentity{identity->get<std::string>(), score, source.generation});
  }
}

void CameraPoseStore::ResolveFaceIdentities(Snapshot& snapshot) {
  snapshot.face_identities.clear();
  const auto persons = snapshot.response.find("persons");
  if (persons == snapshot.response.end() || !persons->is_array()) return;
  const auto counts = TrackCounts(*persons);
  for (auto cache = face_identity_cache_.begin(); cache != face_identity_cache_.end();) {
    const auto current = counts.find(cache->first);
    if (current == counts.end() || current->second != 1) {
      cache = face_identity_cache_.erase(cache);
    } else {
      ++cache;
    }
  }
  for (const auto& person : *persons) {
    if (!person.is_object() || !person.contains("bbox")) continue;
    const auto track = ExplicitTrack(person);
    if (!track || counts.at(*track) != 1) continue;
    auto cached = face_identity_cache_.find(*track);
    if (cached == face_identity_cache_.end()) continue;
    snapshot.face_identities.insert_or_assign(
        *track,
        FaceIdentity{cached->second.identity, cached->second.similarity,
                     cached->second.source_generation});
  }
}

void CameraPoseStore::Publish(Snapshot snapshot) {
  ResolveFaceIdentities(snapshot);
  if (snapshot.face_completed_at) {
    const auto age = std::chrono::steady_clock::now() - *snapshot.face_completed_at;
    if (age <= kFaceObservationTtl) {
      snapshot.face_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(age).count();
    } else {
      ClearFaceObservation(snapshot);
    }
  }
  generation_ = std::max(generation_, snapshot.generation);
  snapshot.sequence = ++sequence_;
  latest_ = snapshot;
  recent_.push_back(std::move(snapshot));
  while (recent_.size() > 4) recent_.pop_front();
}

void CameraPoseStore::UpdateRecent() {
  if (!latest_) return;
  const auto recent = std::find_if(recent_.begin(), recent_.end(), [&](const Snapshot& snapshot) {
    return snapshot.sequence == latest_->sequence;
  });
  if (recent != recent_.end()) *recent = *latest_;
}

std::optional<CameraPoseStore::Snapshot> CameraPoseStore::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = latest_;
  if (snapshot && snapshot->face_completed_at &&
      std::chrono::steady_clock::now() - *snapshot->face_completed_at > kFaceObservationTtl) {
    ClearFaceObservation(*snapshot);
  }
  if (snapshot && snapshot->pose_observed_at) {
    snapshot->pose_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - *snapshot->pose_observed_at)
                                .count();
  }
  return snapshot;
}

CameraPoseStore::FaceSchedulingState CameraPoseStore::FaceRequestState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  FaceSchedulingState state;
  if (!latest_) return state;
  state.human_sequence = latest_->human.sequence;
  state.human_complete = latest_->human.status == Status::kComplete;
  if (state.human_complete) {
    state.candidate_present = HasFaceCandidate(*latest_);
  }
  state.identity_known = HasKnownFaceIdentity(*latest_);
  return state;
}

std::optional<CameraPoseStore::Snapshot> CameraPoseStore::Find(std::string_view device_id,
                                                               std::uint64_t sequence,
                                                               std::uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  exact_followups_.erase(
      std::remove_if(exact_followups_.begin(), exact_followups_.end(),
                     [now](const ExactFollowup& entry) { return entry.expires_at <= now; }),
      exact_followups_.end());
  const auto found = std::find_if(recent_.begin(), recent_.end(), [&](const Snapshot& snapshot) {
    return snapshot.device_id == device_id && snapshot.sequence == sequence &&
           snapshot.generation == generation;
  });
  if (found != recent_.end()) return *found;
  const auto retained = std::find_if(exact_followups_.begin(), exact_followups_.end(),
                                     [&](const ExactFollowup& entry) {
    const auto& snapshot = entry.snapshot;
    return snapshot.device_id == device_id && snapshot.sequence == sequence &&
           snapshot.generation == generation;
  });
  return retained == exact_followups_.end() ? std::nullopt
                                            : std::optional<Snapshot>(retained->snapshot);
}

void CameraPoseStore::RetainExactForFollowup(Snapshot snapshot) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  exact_followups_.erase(
      std::remove_if(exact_followups_.begin(), exact_followups_.end(),
                     [&](const ExactFollowup& entry) {
                       return entry.expires_at <= now ||
                              (entry.snapshot.device_id == snapshot.device_id &&
                               entry.snapshot.sequence == snapshot.sequence &&
                               entry.snapshot.generation == snapshot.generation);
                     }),
      exact_followups_.end());
  exact_followups_.push_back({std::move(snapshot), now + std::chrono::seconds(5)});
  while (exact_followups_.size() > 8) exact_followups_.pop_front();
}

void CameraPoseStore::ReleaseExactFollowup(std::string_view device_id, std::uint64_t sequence,
                                            std::uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  exact_followups_.erase(
      std::remove_if(exact_followups_.begin(), exact_followups_.end(),
                     [&](const ExactFollowup& entry) {
                       const auto& snapshot = entry.snapshot;
                       return snapshot.device_id == device_id && snapshot.sequence == sequence &&
                              snapshot.generation == generation;
                     }),
      exact_followups_.end());
}

void CameraPoseStore::SetUpdatedCallback(Updated updated) {
  std::lock_guard<std::mutex> lock(mutex_);
  updated_ = std::move(updated);
}

CameraPosePump::CameraPosePump(Runtime& models, Capture capture, CameraPoseStore& store,
                               std::string model_id, std::string device_id,
                               std::chrono::milliseconds interval, std::string face_model_id,
                               std::string pose_model_id, std::string gemma_model_id)
    : models_(models),
      capture_(std::move(capture)),
      store_(store),
      model_id_(std::move(model_id)),
      device_id_(std::move(device_id)),
      pose_model_id_(std::move(pose_model_id)),
      face_model_id_(std::move(face_model_id)),
      gemma_model_id_(std::move(gemma_model_id)),
      interval_(interval) {
  if (capture_ == nullptr) throw std::invalid_argument("camera pose capture callback is required");
  if (model_id_.empty()) throw std::invalid_argument("camera pose model id is required");
  if (device_id_.empty()) throw std::invalid_argument("camera pose device id is required");
  if (interval_ <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("camera pose interval must be positive");
  }
  const ModelConfig* pose_model = models_.Find(model_id_);
  if (pose_model == nullptr) throw std::invalid_argument("camera pose model is not configured");
  if (pose_model_id_.empty() && pose_model->capability != Capability::kHuman) {
    throw std::invalid_argument("camera human model must have human capability");
  }
  if (!pose_model_id_.empty()) {
    const ModelConfig* enrichment_model = models_.Find(pose_model_id_);
    const bool typed = pose_model->capability == Capability::kHuman &&
                       enrichment_model != nullptr &&
                       enrichment_model->capability == Capability::kHumanPose;
    const bool legacy = (pose_model->capability == Capability::kPose ||
                         pose_model->capability == Capability::kPosture) &&
                        enrichment_model != nullptr &&
                        enrichment_model->capability == Capability::kDetection;
    if (!typed && !legacy) {
      throw std::invalid_argument("camera pose enrichment model must be human-pose");
    }
    detection_first_ = legacy;
  }
  if (!face_model_id_.empty()) {
    const ModelConfig* face_model = models_.Find(face_model_id_);
    if (face_model == nullptr || face_model->capability != Capability::kFace) {
      throw std::invalid_argument("camera face model must have face capability");
    }
  }
  if (!gemma_model_id_.empty()) {
    const ModelConfig* gemma_model = models_.Find(gemma_model_id_);
    if (gemma_model == nullptr ||
        models_.Resolve(gemma_model_id_, Capability::kPostureClassification) !=
            ModelReferenceState::kEnabled) {
      throw std::invalid_argument("camera Gemma posture model must have gemma-posture capability");
    }
  }
  try {
    worker_ = std::thread(&CameraPosePump::Run, this);
    pose_worker_ = std::thread(&CameraPosePump::RunPose, this);
    if (!pose_model_id_.empty()) {
      enrichment_worker_ = std::thread(&CameraPosePump::RunPoseEnrichment, this);
    }
    if (!face_model_id_.empty()) face_worker_ = std::thread(&CameraPosePump::RunFace, this);
    if (!gemma_model_id_.empty()) gemma_worker_ = std::thread(&CameraPosePump::RunGemma, this);
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wakeup_.notify_all();
    pose_wakeup_.notify_all();
    face_wakeup_.notify_all();
    enrichment_wakeup_.notify_all();
    gemma_wakeup_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (pose_worker_.joinable()) pose_worker_.join();
    if (enrichment_worker_.joinable()) enrichment_worker_.join();
    if (face_worker_.joinable()) face_worker_.join();
    if (gemma_worker_.joinable()) gemma_worker_.join();
    throw;
  }
}

CameraPosePump::~CameraPosePump() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
  }
  wakeup_.notify_all();
  pose_wakeup_.notify_all();
  face_wakeup_.notify_all();
  enrichment_wakeup_.notify_all();
  gemma_wakeup_.notify_all();
  worker_.join();
  pose_worker_.join();
  if (enrichment_worker_.joinable()) enrichment_worker_.join();
  if (face_worker_.joinable()) face_worker_.join();
  if (gemma_worker_.joinable()) gemma_worker_.join();
}

void CameraPosePump::SetPoseEnabled(bool enabled, std::chrono::milliseconds minimum_interval) {
  std::lock_guard<std::mutex> lock(mutex_);
  pose_enabled_ = enabled && !pose_model_id_.empty();
  pose_minimum_interval_ = std::max(minimum_interval, std::chrono::milliseconds::zero());
  if (pose_enabled_) {
    next_pose_request_ = std::chrono::steady_clock::time_point{};
  } else {
    pending_enrichment_.reset();
  }
}

void CameraPosePump::SetFaceEnabled(bool enabled, std::chrono::milliseconds minimum_interval) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool was_enabled = face_enabled_;
  face_enabled_ = enabled && !face_model_id_.empty();
  face_minimum_interval_ = std::max(minimum_interval, std::chrono::milliseconds::zero());
  if (face_enabled_) {
    next_face_request_ = std::chrono::steady_clock::time_point{};
    if (!was_enabled) {
      last_face_human_sequence_ = 0;
      face_candidate_present_ = false;
      face_identity_known_ = false;
      next_face_wait_log_ = std::chrono::steady_clock::time_point{};
    }
  } else {
    pending_face_.reset();
    face_candidate_present_ = false;
    face_identity_known_ = false;
  }
}

void CameraPosePump::Run() {
  auto logger = eos::lib::GetLogger("models.camera_pose");
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_pose_started",
                     {{"model_id", model_id_}, {"interval_ms", interval_.count()}});
  auto next_capture = std::chrono::steady_clock::now();
  std::optional<std::int64_t> last_source_timestamp_ns;
  std::vector<std::uint8_t> last_jpeg;
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    lock.unlock();
    try {
      const std::optional<hardware::CameraTelemetry> frame = capture_();
      if (frame && frame->width_px > 0 && frame->height_px > 0 && !frame->jpeg.empty()) {
        const bool duplicate =
            (frame->source_timestamp_ns && last_source_timestamp_ns &&
             frame->source_timestamp_ns == last_source_timestamp_ns) ||
            (!frame->source_timestamp_ns && !last_source_timestamp_ns && frame->jpeg == last_jpeg);
        if (duplicate) {
          lock.lock();
          next_capture += interval_;
          if (next_capture < std::chrono::steady_clock::now()) {
            next_capture = std::chrono::steady_clock::now();
          }
          wakeup_.wait_until(lock, next_capture, [this] { return stopping_; });
          continue;
        }
        last_source_timestamp_ns = frame->source_timestamp_ns;
        last_jpeg = frame->jpeg;
        auto source_frame = *frame;
        if (!source_frame.source_timestamp_ns) {
          source_frame.source_timestamp_ns = MonotonicNowNs();
          source_frame.source_clock = hardware::SourceClockKind::kSteadyBoot;
          source_frame.source_clock_id = "eos-camera-pose";
        }
        const std::uint64_t generation = store_.NextGeneration();
        const auto captured_at = std::chrono::steady_clock::now();
        bool pose_enabled = false;
        bool pose_requested = false;
        bool face_enabled = false;
        bool face_requested = false;
        bool face_candidate_changed = false;
        bool face_identity_changed = false;
        bool face_wait_log_due = false;
        bool face_candidate_present = false;
        bool face_identity_known = false;
        std::chrono::milliseconds face_request_interval{};
        const auto face_state = store_.FaceRequestState();
        {
          std::lock_guard<std::mutex> job_lock(mutex_);
          const auto now = std::chrono::steady_clock::now();
          pose_enabled = pose_enabled_;
          pose_requested = pose_enabled && now >= next_pose_request_;
          if (pose_requested) next_pose_request_ = now + pose_minimum_interval_;
          face_enabled = face_enabled_;
          if (face_enabled && face_state.human_complete &&
              face_state.human_sequence != last_face_human_sequence_) {
            last_face_human_sequence_ = face_state.human_sequence;
            const bool has_candidate = face_state.candidate_present;
            face_candidate_changed = has_candidate != face_candidate_present_;
            face_candidate_present_ = has_candidate;
            if (!has_candidate) {
              next_face_request_ = std::chrono::steady_clock::time_point{};
              face_identity_known_ = false;
            }
          }
          if (face_enabled && face_candidate_present_ &&
              (face_state.human_complete || face_state.identity_known)) {
            const bool known_identity = face_state.identity_known;
            face_identity_changed = known_identity != face_identity_known_;
            face_identity_known_ = known_identity;
          }
          face_candidate_present = face_candidate_present_;
          face_identity_known = face_identity_known_;
          if (face_candidate_present) {
            const auto desired_interval = face_identity_known ? kFaceRecognizedRefreshInterval
                                                              : kFaceUnresolvedRequestInterval;
            face_request_interval = std::max(face_minimum_interval_, desired_interval);
            face_requested = face_enabled && now >= next_face_request_;
            if (face_requested) next_face_request_ = now + face_request_interval;
          } else if (face_enabled) {
            next_face_request_ = std::chrono::steady_clock::time_point{};
          }
          face_wait_log_due = face_enabled && !face_candidate_present && now >= next_face_wait_log_;
          if (face_wait_log_due) next_face_wait_log_ = now + kFaceMetricsWindow;
        }
        if (face_enabled && face_candidate_changed) {
          eos::lib::LogEvent(
              eos::lib::GetLogger("models.camera_face"), spdlog::level::info,
              face_candidate_present ? "camera_face_candidate_detected"
                                     : "camera_face_candidate_missing",
              {{"model_id", face_model_id_}},
              face_candidate_present
                  ? "ArcFace detected a tracked person; recognition requests are starting"
                  : "ArcFace requests paused because no tracked person is present");
        }
        if (face_enabled && face_identity_changed && face_identity_known) {
          eos::lib::LogEvent(
              eos::lib::GetLogger("models.camera_face"), spdlog::level::info,
              "camera_face_identity_resolved", {{"model_id", face_model_id_}},
              "ArcFace matched a registered identity; refresh requests are now slower");
        }
        if (face_candidate_changed && face_candidate_present) {
          eos::lib::LogEvent(eos::lib::GetLogger("models.camera_face"), spdlog::level::info,
                             "camera_face_schedule_changed",
                             {{"model_id", face_model_id_},
                              {"request_interval_ms", face_request_interval.count()},
                              {"identity_known", face_identity_known}},
                             "ArcFace recognition cadence selected for the current person");
        }
        if (face_wait_log_due) {
          eos::lib::LogEvent(
              eos::lib::GetLogger("models.camera_face"), spdlog::level::info,
              "camera_face_waiting_for_person", {{"model_id", face_model_id_}},
              "ArcFace is enabled but waiting for a tracked person before sending a request");
        }
        if (face_requested) {
          store_.SetSource(MakeSourceSnapshot(source_frame, generation, model_id_, device_id_),
                           !pose_model_id_.empty(), pose_requested, true, true);
        }
        if (face_requested) {
          const auto queued_at = std::chrono::steady_clock::now();
          auto jpeg_b64 = std::make_shared<const std::string>(Base64Encode(source_frame.jpeg));
          bool pending_replaced = false;
          std::uint64_t replacement_count = 0;
          {
            std::lock_guard<std::mutex> job_lock(mutex_);
            ++face_metrics_.queued;
            if (pending_face_) {
              pending_replaced = true;
              replacement_count = ++face_metrics_.replaced;
            }
            pending_face_ = AuxiliaryJob{.generation = generation,
                                         .jpeg_b64 = std::move(jpeg_b64),
                                         .captured_at = captured_at,
                                         .queued_at = queued_at};
            face_wakeup_.notify_one();
          }
          eos::lib::LogEvent(
              eos::lib::GetLogger("models.camera_face"), spdlog::level::debug,
              "camera_face_request_queued",
              {{"model_id", face_model_id_},
               {"generation", generation},
               {"request_interval_ms", face_request_interval.count()},
               {"frame_age_ms",
                std::chrono::duration_cast<std::chrono::milliseconds>(queued_at - captured_at)
                    .count()},
               {"pending_frame_replaced", pending_replaced},
               {"total_pending_replacements", replacement_count}},
              "ArcFace recognition frame queued");
        }
        {
          std::lock_guard<std::mutex> job_lock(mutex_);
          ++metrics_.captured;
          ++metrics_.submitted;
          if (pending_pose_) ++metrics_.replaced;
          pending_pose_ = PoseJob{.generation = generation,
                                  .frame = std::move(source_frame),
                                  .pose_requested = pose_requested,
                                  .captured_at = captured_at};
          pose_wakeup_.notify_one();
        }
      }
    } catch (const std::exception& error) {
      eos::lib::LogEvent(logger, spdlog::level::warn, "camera_pose_capture_failed",
                         {{"model_id", model_id_}, {"reason", error.what()}});
    }
    lock.lock();
    next_capture += interval_;
    if (next_capture < std::chrono::steady_clock::now()) {
      next_capture = std::chrono::steady_clock::now();
    }
    wakeup_.wait_until(lock, next_capture, [this] { return stopping_; });
  }
  lock.unlock();
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_pose_stopped", {{"model_id", model_id_}});
}

void CameraPosePump::RunPose() {
  auto logger = eos::lib::GetLogger("models.camera_pose_inference");
  std::optional<std::size_t> last_logged_person_count;
  while (true) {
    PoseJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      pose_wakeup_.wait(lock, [this] { return stopping_ || pending_pose_.has_value(); });
      if (stopping_) break;
      job = std::move(*pending_pose_);
      pending_pose_.reset();
    }
    auto& frame = job.frame;
    store_.SetSource(MakeSourceSnapshot(frame, job.generation, model_id_, device_id_),
                     !pose_model_id_.empty(), job.pose_requested, !face_model_id_.empty(), false);
    const auto encoding_started = std::chrono::steady_clock::now();
    const std::int64_t source_timestamp_ns = *frame.source_timestamp_ns;
    job.jpeg_b64 = std::make_shared<const std::string>(Base64Encode(frame.jpeg));
    const auto encoding_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - encoding_started)
                                 .count();
    nlohmann::json request = {
        {"input",
         {{"data_b64", *job.jpeg_b64}, {"filename", "camera.jpg"}, {"media_type", "image/jpeg"}}}};
    const auto call_started = std::chrono::steady_clock::now();
    CallResult result =
        models_.Call(detection_first_ ? pose_model_id_ : model_id_, request, [this] {
          std::lock_guard<std::mutex> lock(mutex_);
          return stopping_;
        });
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - call_started)
                                 .count();
    if (result.status != CallResult::Status::kCompleted) {
      store_.SetHumanUnavailable(job.generation);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stable_largest_track_.clear();
        stable_largest_frames_ = 0;
        stable_largest_bbox_ = nullptr;
      }
      eos::lib::LogEvent(logger, spdlog::level::warn, "camera_pose_failed",
                         {{"model_id", model_id_},
                          {"generation", job.generation},
                          {"status", static_cast<int>(result.status)},
                          {"reason", result.message}});
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.failed;
      continue;
    }
    const std::size_t person_count = result.response.value("person_count", 0);
    if (!last_logged_person_count || *last_logged_person_count != person_count) {
      eos::lib::LogEvent(logger, spdlog::level::debug, "camera_pose_summary",
                         {{"model_id", model_id_},
                          {"generation", job.generation},
                          {"source_timestamp_ns", source_timestamp_ns},
                          {"width_px", frame.width_px},
                          {"height_px", frame.height_px},
                          {"depth_available", !frame.depth_png.empty() || !frame.depth_u16.empty()},
                          {"person_count", person_count},
                          {"duration_ms", duration_ms}});
      last_logged_person_count = person_count;
    }
    nlohmann::json response = std::move(result.response);
    if (detection_first_) {
      nlohmann::json persons = nlohmann::json::array();
      for (const auto& object : response.at("objects")) {
        persons.push_back({{"track_id", object.at("track_id")},
                           {"bbox", object.at("bbox")},
                           {"confidence", object.at("confidence")}});
      }
      response = {{"persons", std::move(persons)}};
      response["person_count"] = response.at("persons").size();
    }
    AddDepthDistances(response, frame);
    const nlohmann::json raw_persons = response.at("persons");
    if (job.pose_requested) job.human_response = response;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.completed;
      metrics_.encoding_ms += encoding_ms;
      metrics_.inference_ms += duration_ms;
      metrics_.total_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - job.captured_at)
                               .count();
      const auto elapsed = std::chrono::steady_clock::now() - metrics_.window_started;
      if (elapsed >= std::chrono::seconds(1)) {
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const auto finished = metrics_.completed + metrics_.failed;
        eos::lib::LogEvent(
            logger, spdlog::level::debug, "camera_pose_throughput",
            {{"model_id", model_id_},
             {"captured_hz", metrics_.captured / seconds},
             {"submitted_hz", metrics_.submitted / seconds},
             {"replaced", metrics_.replaced},
             {"completed_hz", metrics_.completed / seconds},
             {"failed", metrics_.failed},
             {"encoding_ms",
              finished ? metrics_.encoding_ms / static_cast<std::int64_t>(finished) : 0},
             {"inference_ms",
              finished ? metrics_.inference_ms / static_cast<std::int64_t>(finished) : 0},
             {"total_ms", finished ? metrics_.total_ms / static_cast<std::int64_t>(finished) : 0}});
        metrics_ = Metrics{};
      }
    }
    if (!store_.SetHuman(job.generation, std::move(response))) continue;
    if (!gemma_model_id_.empty()) {
      const auto current = store_.Latest();
      std::string largest_track;
      nlohmann::json largest_bbox;
      double largest_area = 0.0;
      double largest_confidence = 0.0;
      std::string largest_signature;
      if (current && current->generation == job.generation) {
        for (const auto& person : raw_persons) {
          if (!person.is_object() || !person.contains("bbox")) continue;
          const auto track = ExplicitTrack(person);
          double bbox[4];
          if (!track || !ReadBbox(person.at("bbox"), bbox)) continue;
          const double area = (bbox[2] - bbox[0]) * (bbox[3] - bbox[1]);
          if (area > largest_area) {
            largest_area = area;
            largest_track = *track;
            largest_bbox = person.at("bbox");
            largest_confidence = person.value("confidence", 0.0);
            largest_signature = person.value("pose", "unknown") + std::string(":") +
                person.value("posture_state", "") + ":" +
                person.value("action", "unknown");
          }
        }
      }
      if (largest_confidence < kGemmaMinimumTrackConfidence ||
          largest_area < kGemmaMinimumImageArea * frame.width_px * frame.height_px)
        largest_track.clear();
      std::lock_guard<std::mutex> lock(mutex_);
      if (current && current->response.contains("persons")) {
        const auto& persons = current->response.at("persons");
        for (auto it = gemma_completed_tracks_.begin(); it != gemma_completed_tracks_.end();) {
          const bool present = std::any_of(persons.begin(), persons.end(), [&](const auto& person) {
            return person.is_object() && ExplicitTrack(person) == *it;
          });
          if (!present) it = gemma_completed_tracks_.erase(it);
          else ++it;
        }
        for (auto it = gemma_track_signatures_.begin(); it != gemma_track_signatures_.end();) {
          const bool present = std::any_of(persons.begin(), persons.end(), [&](const auto& person) {
            return person.is_object() && ExplicitTrack(person) == it->first;
          });
          if (!present) {
            gemma_refresh_pending_.erase(it->first);
            it = gemma_track_signatures_.erase(it);
          }
          else ++it;
        }
      }
      const auto now = std::chrono::steady_clock::now();
      if (!largest_track.empty()) {
        auto [it, inserted] = gemma_track_signatures_.try_emplace(largest_track, largest_signature);
        if (!inserted && it->second != largest_signature) {
          it->second = largest_signature;
          gemma_completed_tracks_.erase(largest_track);
          gemma_refresh_pending_.insert(largest_track);
        }
      }
      if (largest_track.empty()) {
        stable_largest_track_.clear();
        stable_largest_frames_ = 0;
        stable_largest_bbox_ = nullptr;
      } else if (largest_track == stable_largest_track_ &&
                 BoxIou(stable_largest_bbox_, largest_bbox) >= kGemmaMinimumBoxIou) {
        ++stable_largest_frames_;
        stable_largest_bbox_ = largest_bbox;
      } else {
        stable_largest_track_ = largest_track;
        stable_largest_frames_ = 1;
        stable_largest_bbox_ = largest_bbox;
        stable_largest_since_ = now;
      }
      for (auto it = gemma_last_request_.begin(); it != gemma_last_request_.end();) {
        if (now - it->second >= kGemmaRetryInterval) {
          it = gemma_last_request_.erase(it);
        } else {
          ++it;
        }
      }
      const auto previous = gemma_last_request_.find(largest_track);
      if (stable_largest_frames_ >= kGemmaStableFrames &&
          now - stable_largest_since_ >= kGemmaStableDuration &&
          gemma_completed_tracks_.find(largest_track) == gemma_completed_tracks_.end() &&
          !gemma_busy_ && !pending_gemma_ &&
          (previous == gemma_last_request_.end() ||
           now - previous->second >= (gemma_refresh_pending_.count(largest_track)
                                         ? kGemmaChangeRetryInterval : kGemmaRetryInterval))) {
        gemma_last_request_[largest_track] = now;
        gemma_refresh_pending_.erase(largest_track);
        gemma_busy_ = true;
        pending_gemma_ = GemmaJob{job.generation, largest_track, std::move(largest_bbox),
                                  job.jpeg_b64, store_.TrackInputRevision(largest_track)};
        gemma_wakeup_.notify_one();
      }
    }
    if (job.pose_requested) {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_enrichment_ = job;
      enrichment_wakeup_.notify_one();
    }
  }
}

void CameraPosePump::RunPoseEnrichment() {
  constexpr auto kDepthInterval = std::chrono::milliseconds(200);
  auto next_depth_request = std::chrono::steady_clock::time_point{};
  auto logger = eos::lib::GetLogger("models.camera_pose_enrichment");
  while (true) {
    PoseJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      enrichment_wakeup_.wait(lock,
                              [this] { return stopping_ || pending_enrichment_.has_value(); });
      if (stopping_) break;
      job = std::move(*pending_enrichment_);
      pending_enrichment_.reset();
    }
    nlohmann::json persons = nlohmann::json::array();
    for (const auto& detected : job.human_response.at("persons")) {
      nlohmann::json tracked = nlohmann::json::object();
      for (const char* key : {"track_id", "bbox", "confidence", "distance_m"}) {
        if (detected.contains(key)) tracked[key] = detected.at(key);
      }
      persons.push_back(std::move(tracked));
    }
    nlohmann::json request = {
        {"input",
         {{"data_b64", *job.jpeg_b64}, {"filename", "camera.jpg"}, {"media_type", "image/jpeg"}}},
        {"persons", std::move(persons)}};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_depth_request) {
      next_depth_request = now + kDepthInterval;
      const auto& frame = job.frame;
      const auto* model = models_.Find(detection_first_ ? model_id_ : pose_model_id_);
      if (model && std::isfinite(frame.depth_scale) && frame.depth_scale > 0.0 &&
          frame.width_px > 0 && frame.height_px > 0 &&
          frame.depth_u16.size() == static_cast<std::size_t>(frame.width_px) * frame.height_px &&
          frame.depth_intrinsics && frame.depth_intrinsics->width_px == frame.width_px &&
          frame.depth_intrinsics->height_px == frame.height_px) {
        auto png = EncodeAlignedDepthPng(frame.depth_u16, frame.width_px, frame.height_px);
        if (!png.empty()) {
          job.depth_b64 = std::make_shared<const std::string>(Base64Encode(png));
          request["depth_input"] = {{"data_b64", *job.depth_b64},
                                    {"filename", "camera-depth.png"},
                                    {"media_type", "image/png"}};
          request["depth_scale"] = frame.depth_scale;
          const auto& intrinsics = *frame.depth_intrinsics;
          request["depth_intrinsics"] = {
              {"width", intrinsics.width_px},
              {"height", intrinsics.height_px},
              {"fx", intrinsics.fx},
              {"fy", intrinsics.fy},
              {"ppx", intrinsics.ppx},
              {"ppy", intrinsics.ppy},
              {"distortion_model", intrinsics.distortion_model},
              {"coeffs", intrinsics.distortion_coefficients}};
          if (request.dump().size() > model->max_request_bytes) {
            request.erase("depth_input");
            request.erase("depth_scale");
            request.erase("depth_intrinsics");
            eos::lib::LogEvent(
                logger, spdlog::level::warn, "camera_pose_depth_oversize",
                {{"generation", job.generation}, {"limit_bytes", model->max_request_bytes}});
          } else {
            eos::lib::LogEvent(logger, spdlog::level::debug, "camera_pose_depth_attached",
                               {{"generation", job.generation},
                                {"png_bytes", png.size()},
                                {"request_bytes", request.dump().size()}});
          }
        } else {
          eos::lib::LogEvent(logger, spdlog::level::warn, "camera_pose_depth_encode_failed",
                             {{"generation", job.generation}});
        }
      } else {
        eos::lib::LogEvent(logger, spdlog::level::trace, "camera_pose_depth_unavailable",
                           {{"generation", job.generation}});
      }
    }
    CallResult result =
        models_.Call(detection_first_ ? model_id_ : pose_model_id_, request, [this] {
          std::lock_guard<std::mutex> lock(mutex_);
          return stopping_ || !pose_enabled_;
        });
    if (result.status == CallResult::Status::kCompleted) {
      AddAlignedKeypointDepths(result.response, job.frame);
      if (!store_.MergePose(job.generation, std::move(result.response),
                            job.frame.source_timestamp_ns, job.frame.source_clock,
                            job.frame.source_clock_id, job.captured_at)) {
        eos::lib::LogEvent(logger, spdlog::level::debug, "camera_pose_merge_skipped",
                           {{"generation", job.generation}});
      }
    } else {
      eos::lib::LogEvent(logger, spdlog::level::warn, "camera_pose_enrichment_failed",
                         {{"generation", job.generation},
                          {"status", static_cast<int>(result.status)},
                          {"reason", result.message}});
      store_.SetPoseUnavailable(job.generation);
    }
  }
}

void CameraPosePump::RunGemma() {
  auto logger = eos::lib::GetLogger("models.camera_gemma_posture");
  while (true) {
    GemmaJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      gemma_wakeup_.wait(lock, [this] { return stopping_ || pending_gemma_.has_value(); });
      if (stopping_) break;
      job = std::move(*pending_gemma_);
      pending_gemma_.reset();
    }
    const nlohmann::json request = {
        {"input", {{"data_b64", *job.jpeg_b64}, {"filename", "camera.jpg"},
                    {"media_type", "image/jpeg"}}},
        {"bbox", job.bbox}};
    eos::lib::LogEvent(logger, spdlog::level::debug, "camera_gemma_action_requested",
                       {{"model_id", gemma_model_id_}, {"track_id", job.track_id},
                        {"generation", job.generation}});
    const auto started_at = std::chrono::steady_clock::now();
    const CallResult result = models_.Call(gemma_model_id_, request, [this] {
      std::lock_guard<std::mutex> lock(mutex_);
      return stopping_;
    });
    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_at)
                                .count();
    if (result.status == CallResult::Status::kCompleted) {
      const std::string posture_state = result.response.at("posture_state").get<std::string>();
      const std::string action = result.response.at("action").get<std::string>();
      const bool applied = store_.SetGemmaPosture(job.generation, job.track_id,
                                                 posture_state, action, job.input_revision);
      eos::lib::LogEvent(logger, spdlog::level::info, "camera_gemma_action_completed",
                         {{"model_id", gemma_model_id_}, {"track_id", job.track_id},
                          {"generation", job.generation}, {"latency_ms", latency_ms},
                          {"posture_state", posture_state}, {"action", action},
                          {"applied", applied}});
      if (applied) {
        std::lock_guard<std::mutex> lock(mutex_);
        gemma_completed_tracks_.insert(job.track_id);
      }
    } else {
      eos::lib::LogEvent(logger, spdlog::level::warn, "camera_gemma_action_failed",
                         {{"model_id", gemma_model_id_}, {"status", static_cast<int>(result.status)},
                          {"track_id", job.track_id}, {"generation", job.generation},
                          {"latency_ms", latency_ms}, {"reason", result.message}});
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      gemma_busy_ = false;
    }
  }
}

void CameraPosePump::RunFace() {
  auto logger = eos::lib::GetLogger("models.camera_face");
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_face_started",
                     {{"model_id", face_model_id_}}, "ArcFace recognition worker started");
  while (true) {
    AuxiliaryJob job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      face_wakeup_.wait(lock, [this] { return stopping_ || pending_face_.has_value(); });
      if (stopping_) break;
      job = std::move(*pending_face_);
      pending_face_.reset();
    }
    const auto request_started = std::chrono::steady_clock::now();
    const auto queue_wait_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(request_started - job.queued_at)
            .count();
    const auto frame_age_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(request_started - job.captured_at)
            .count();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++face_metrics_.started;
      face_metrics_.last_queue_wait_ms = queue_wait_ms;
      face_metrics_.last_frame_age_ms = frame_age_ms;
    }
    eos::lib::LogEvent(logger, spdlog::level::debug, "camera_face_request_started",
                       {{"model_id", face_model_id_},
                        {"generation", job.generation},
                        {"queue_wait_ms", queue_wait_ms},
                        {"frame_age_ms", frame_age_ms}},
                       "ArcFace recognition request started");

    bool stale_before_send = request_started - job.captured_at > kFaceRequestMaximumAge;
    CallResult result;
    std::int64_t duration_ms = 0;
    if (!stale_before_send) {
      result = models_.Call(face_model_id_,
                            {{"operation", "recognize"},
                             {"input",
                              {{"data_b64", *job.jpeg_b64},
                               {"filename", "camera.jpg"},
                               {"media_type", "image/jpeg"}}}},
                            [this] {
                              std::lock_guard<std::mutex> lock(mutex_);
                              return stopping_ || !face_enabled_;
                            });
      duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - request_started)
                        .count();
    }
    const auto completed_at = std::chrono::steady_clock::now();
    const auto completed_frame_age_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(completed_at - job.captured_at)
            .count();
    const bool stale_result = !stale_before_send &&
                              result.status == CallResult::Status::kCompleted &&
                              completed_at - job.captured_at > kFaceRequestMaximumAge;
    bool face_enabled = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      face_enabled = face_enabled_;
      face_metrics_.last_inference_ms = duration_ms;
      face_metrics_.last_frame_age_ms = completed_frame_age_ms;
      if (stale_before_send || stale_result) {
        ++face_metrics_.stale;
      } else if (result.status == CallResult::Status::kCompleted) {
        ++face_metrics_.completed;
      } else {
        ++face_metrics_.failed;
      }
    }

    if (stale_before_send || stale_result) {
      store_.SetFaceUnavailable(job.generation);
      eos::lib::LogEvent(
          logger, spdlog::level::warn, "camera_face_result_discarded_stale",
          {{"model_id", face_model_id_},
           {"generation", job.generation},
           {"frame_age_ms", completed_frame_age_ms},
           {"inference_duration_ms", duration_ms}},
          "ArcFace result discarded because its camera frame was older than 3 seconds");
    } else if (result.status == CallResult::Status::kCompleted) {
      const nlohmann::json data = result.response.value("data", nlohmann::json::object());
      const std::size_t face_count = data.value("face_count", 0);
      store_.SetFace(job.generation, std::move(result.response));
      eos::lib::LogEvent(logger, spdlog::level::debug, "camera_face_request_completed",
                         {{"model_id", face_model_id_},
                          {"generation", job.generation},
                          {"face_count", face_count},
                          {"queue_wait_ms", queue_wait_ms},
                          {"inference_duration_ms", duration_ms},
                          {"frame_age_ms", completed_frame_age_ms}},
                         "ArcFace recognition request completed");
    } else {
      store_.SetFaceUnavailable(job.generation);
      if (face_enabled) {
        const std::string status(CallStatusName(result.status));
        eos::lib::LogEvent(logger, spdlog::level::warn, "camera_face_request_failed",
                           {{"model_id", face_model_id_},
                            {"generation", job.generation},
                            {"status", status},
                            {"queue_wait_ms", queue_wait_ms},
                            {"inference_duration_ms", duration_ms},
                            {"frame_age_ms", completed_frame_age_ms}},
                           "ArcFace recognition request failed with status " + status);
      }
    }

    nlohmann::json summary_fields;
    bool emit_summary = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (completed_at - face_metrics_.window_started >= kFaceMetricsWindow) {
        summary_fields = {{"model_id", face_model_id_},
                          {"window_s", 30},
                          {"queued", face_metrics_.queued},
                          {"started", face_metrics_.started},
                          {"completed", face_metrics_.completed},
                          {"failed", face_metrics_.failed},
                          {"stale", face_metrics_.stale},
                          {"pending_replaced", face_metrics_.replaced},
                          {"last_queue_wait_ms", face_metrics_.last_queue_wait_ms},
                          {"last_inference_ms", face_metrics_.last_inference_ms},
                          {"last_frame_age_ms", face_metrics_.last_frame_age_ms}};
        face_metrics_.queued = 0;
        face_metrics_.replaced = 0;
        face_metrics_.started = 0;
        face_metrics_.completed = 0;
        face_metrics_.failed = 0;
        face_metrics_.stale = 0;
        face_metrics_.window_started = completed_at;
        emit_summary = true;
      }
    }
    if (emit_summary) {
      const std::string summary =
          "ArcFace requests in the last 30 seconds: queued=" +
          std::to_string(summary_fields.at("queued").get<std::uint64_t>()) +
          " completed=" + std::to_string(summary_fields.at("completed").get<std::uint64_t>()) +
          " failed=" + std::to_string(summary_fields.at("failed").get<std::uint64_t>()) +
          " stale=" + std::to_string(summary_fields.at("stale").get<std::uint64_t>()) +
          " replaced=" +
          std::to_string(summary_fields.at("pending_replaced").get<std::uint64_t>()) +
          "; last queue wait=" +
          std::to_string(summary_fields.at("last_queue_wait_ms").get<std::int64_t>()) +
          " ms, inference=" +
          std::to_string(summary_fields.at("last_inference_ms").get<std::int64_t>()) +
          " ms, frame age=" +
          std::to_string(summary_fields.at("last_frame_age_ms").get<std::int64_t>()) + " ms";
      eos::lib::LogEvent(logger, spdlog::level::info, "camera_face_timing_summary", summary_fields,
                         summary);
    }
  }
  eos::lib::LogEvent(logger, spdlog::level::info, "camera_face_stopped",
                     {{"model_id", face_model_id_}}, "ArcFace recognition worker stopped");
}

}  // namespace eos::runtime::models

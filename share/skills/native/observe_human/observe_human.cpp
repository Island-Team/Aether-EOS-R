#include "src/eos/runtime/skills/builtin/registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "share/skills/native/observe_human/geometry.h"

namespace island::skills::native {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {
constexpr auto kPoll = std::chrono::milliseconds(20);
constexpr std::size_t kStableGenerations = 3;
constexpr double kV1DefaultConfidence = 0.40;
constexpr double kV2DefaultConfidence = 0.40;
constexpr double kV2MinimumCenterYFraction = 13.0 / 24.0;
constexpr double kMinimumBboxIou = 0.5;
constexpr double kBboxAimYRatio = 0.42;
constexpr double kTiltBoundaryToleranceRad = 3.14159265358979323846 / 180;
constexpr double kMaximumTiltDelta = 3.14159265358979323846 / 6;
constexpr double kFollowFilterAlpha = 0.8;
constexpr double kHorizontalDeadband = 0.04;
constexpr double kVerticalDeadband = 0.06;
constexpr double kHorizontalExitDeadband = 0.08;
constexpr double kVerticalExitDeadband = 0.10;
constexpr double kNearCenterLimit = 0.30;
constexpr double kNearCenterPanCapRad = 0.035;
constexpr double kNearCenterTiltCapRad = 0.025;
constexpr double kFollowSpeedRadPerSecond = 0.4;
constexpr double kInitialDownwardTiltDeltaRad = 0.3;
constexpr auto kInitialDownwardObservationHold = std::chrono::seconds(5);

struct Target {
  std::string id;
  std::string registered_name;
  bool fallen_fallback{false};
  bool priority_pose{false};
  std::optional<std::uint64_t> identity_source_generation;
  nlohmann::json bbox;
  double horizontal_error{0};
  double vertical_error{0};
};

struct Frame {
  std::string source;
  std::uint64_t sequence{0};
  std::uint64_t generation{0};
  std::int64_t timestamp_ns{0};
  double width{0};
  double height{0};
  std::optional<double> gimbal_pan_rad;
  std::optional<double> gimbal_tilt_rad;
  std::optional<double> gimbal_pan_rate_rad_s;
  std::optional<double> gimbal_tilt_rate_rad_s;
};

double BboxIou(const nlohmann::json& left, const nlohmann::json& right);

std::optional<Frame> ParseFrame(const nlohmann::json& snapshot) {
  const auto sequence = snapshot.find("source_sequence");
  const auto generation = snapshot.find("source_generation");
  const std::string source = snapshot.value("source", "");
  const double width = snapshot.value("image_width_px", 0.0);
  const double height = snapshot.value("image_height_px", 0.0);
  if (sequence == snapshot.end() || generation == snapshot.end() ||
      !sequence->is_number_integer() || !generation->is_number_integer() || source.empty() ||
      !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0)
    return std::nullopt;
  const auto signed_sequence = sequence->get<std::int64_t>();
  const auto signed_generation = generation->get<std::int64_t>();
  if (signed_sequence <= 0 || signed_generation <= 0) return std::nullopt;
  const auto parsed_sequence = static_cast<std::uint64_t>(signed_sequence);
  const auto parsed_generation = static_cast<std::uint64_t>(signed_generation);
  Frame frame{source, parsed_sequence, parsed_generation,
              snapshot.value("source_timestamp_ns", std::int64_t{0}), width, height};
  const auto source_gimbal = snapshot.find("source_gimbal");
  if (source_gimbal != snapshot.end() && source_gimbal->is_object()) {
    const auto read_finite = [&](const char* key) -> std::optional<double> {
      const auto field = source_gimbal->find(key);
      if (field == source_gimbal->end() || !field->is_number()) return std::nullopt;
      const double value = field->get<double>();
      return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    };
    frame.gimbal_pan_rad = read_finite("pan_rad");
    frame.gimbal_tilt_rad = read_finite("tilt_rad");
    frame.gimbal_pan_rate_rad_s = read_finite("pan_rate_rad_s");
    frame.gimbal_tilt_rate_rad_s = read_finite("tilt_rate_rad_s");
  }
  return frame;
}

// Source publications may carry detections retained from the preceding frame
// while the human detector is still working on this generation.
bool HumanDetectionReady(const nlohmann::json& snapshot, std::uint64_t generation) {
  const auto models = snapshot.find("models");
  if (models == snapshot.end() || !models->is_object()) return true;
  const auto human = models->find("human");
  if (human == models->end() || !human->is_object()) return true;
  const auto source = human->find("source_generation");
  return human->value("status", "") == "complete" &&
         source != human->end() && source->is_number_integer() &&
         source->get<std::int64_t>() >= 0 &&
         static_cast<std::uint64_t>(source->get<std::int64_t>()) == generation;
}

std::optional<Target> FindTarget(const nlohmann::json& snapshot, const Frame& frame,
                                  std::string_view expected_id,
                                  std::string_view expected_registered_name, double confidence,
                                  bool require_registered_identity,
                                  bool allow_unidentified_identity,
                                  std::optional<std::uint64_t> identity_after_generation,
                                  std::optional<double> maximum_tilt,
                                  std::optional<double> fallback_capture_tilt,
                                   const nlohmann::json* continuity_bbox = nullptr,
                                   bool v2 = false,
                                   const std::unordered_set<std::string>* excluded_ids = nullptr) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return std::nullopt;
  std::optional<Target> largest;
  double best_score = 0;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != "human" ||
        detection.value("confidence", 0.0) < confidence)
      continue;
    const std::string id = detection.value("id", "");
    if (id.empty() || (!expected_id.empty() && id != expected_id)) continue;
    std::string registered_name;
    const auto attributes = detection.find("attributes");
    const bool fallen = attributes != detection.end() && attributes->is_object() &&
                         attributes->value("action", "") == "fallen";
    const bool lying = attributes != detection.end() && attributes->is_object() &&
                       attributes->value("pose", "") == "lie";
    if (excluded_ids && excluded_ids->contains(id) && !(v2 ? lying : fallen)) continue;
    if (attributes != detection.end() && attributes->is_object()) {
      const auto name = attributes->find("name");
      if (name != attributes->end() && name->is_string() &&
          !name->get_ref<const std::string&>().empty() &&
           name->get_ref<const std::string&>() != "unknown" &&
           (!v2 || name->get_ref<const std::string&>() != "none"))
        registered_name = name->get<std::string>();
    }
    const bool fallen_fallback = registered_name.empty() && fallen;
    const bool anonymous_fallback = v2 && registered_name.empty() && lying;
    if (v2 && registered_name.empty() && !lying) continue;
    if (require_registered_identity && registered_name.empty() && !fallen_fallback &&
        !anonymous_fallback) continue;
    if (!expected_registered_name.empty()) {
      if (!registered_name.empty() && registered_name != expected_registered_name) continue;
      if (registered_name.empty() && (v2 || !allow_unidentified_identity)) continue;
    }
    std::optional<std::uint64_t> identity_source_generation;
    if (!registered_name.empty()) {
      const auto source_generation = attributes->find("name_source_generation");
      if (source_generation != attributes->end() && source_generation->is_number_integer()) {
        const auto value = source_generation->get<std::int64_t>();
        if (value >= 0) identity_source_generation = static_cast<std::uint64_t>(value);
      }
    }
    if (!fallen_fallback && !anonymous_fallback && identity_after_generation &&
        (!identity_source_generation || *identity_source_generation <= *identity_after_generation))
      continue;
    const auto bbox = detection.find("bbox");
    if (bbox == detection.end() || !bbox->is_array() || bbox->size() != 4) continue;
    double b[4]{};
    bool valid = true;
    for (std::size_t i = 0; i < 4; ++i) {
      if (!bbox->at(i).is_number()) {
        valid = false;
        continue;
      }
      b[i] = bbox->at(i).get<double>();
      valid = valid && std::isfinite(b[i]);
    }
    if (!valid || b[0] < 0 || b[1] < 0 || b[2] > frame.width || b[3] > frame.height ||
        b[0] >= b[2] || b[1] >= b[3])
      continue;
    if (v2 && maximum_tilt && (b[1] + b[3]) / 2 <
                                 frame.height * kV2MinimumCenterYFraction) continue;
    const double aim_y = b[1] + (b[3] - b[1]) * kBboxAimYRatio;
    const double vertical_error = (2 * aim_y - frame.height) / frame.height;
    if (maximum_tilt && !v2) {
      const auto capture_tilt = frame.gimbal_tilt_rad ? frame.gimbal_tilt_rad
                                                      : fallback_capture_tilt;
      if (capture_tilt) {
        const double vertical_fov =
            detail::VerticalFovRad(detail::kHorizontalFovRad, frame.width, frame.height);
        const double required_tilt =
            *capture_tilt - detail::NormalizedImageErrorToAngle(vertical_error, vertical_fov);
        if (required_tilt > *maximum_tilt + kTiltBoundaryToleranceRad) continue;
      }
    }
    const double score = continuity_bbox ? BboxIou(*continuity_bbox, *bbox)
                         : v2              ? 0.0
                                           : (b[2] - b[0]) * (b[3] - b[1]);
    if (continuity_bbox && score < kMinimumBboxIou) continue;
    const bool priority_pose = v2 ? lying : fallen;
    const bool preferred_pose = !continuity_bbox && largest && priority_pose &&
                                !largest->priority_pose;
    const bool inferior_pose = !continuity_bbox && largest && !priority_pose &&
                               largest->priority_pose;
    const bool preferred_name = v2 && !continuity_bbox && largest &&
                                priority_pose == largest->priority_pose &&
                                 !registered_name.empty() && largest->registered_name.empty();
    const bool inferior_identity = v2 && !continuity_bbox && largest &&
                                   priority_pose == largest->priority_pose &&
                                    registered_name.empty() && !largest->registered_name.empty();
    if (preferred_pose ||
        (!inferior_pose &&
          (preferred_name || (!inferior_identity &&
                              (v2 ? (!largest || (continuity_bbox && score > best_score))
                                  : score > best_score))))) {
      best_score = score;
      largest = Target{id, registered_name, fallen_fallback || anonymous_fallback,
                        priority_pose, identity_source_generation, *bbox,
                         (b[0] + b[2] - frame.width) / frame.width, vertical_error};
    }
  }
  return largest;
}

double BboxIou(const nlohmann::json& left, const nlohmann::json& right) {
  const double x1 = std::max(left[0].get<double>(), right[0].get<double>());
  const double y1 = std::max(left[1].get<double>(), right[1].get<double>());
  const double x2 = std::min(left[2].get<double>(), right[2].get<double>());
  const double y2 = std::min(left[3].get<double>(), right[3].get<double>());
  const double intersection = std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
  const auto area = [](const nlohmann::json& box) {
    return (box[2].get<double>() - box[0].get<double>()) *
           (box[3].get<double>() - box[1].get<double>());
  };
  const double total = area(left) + area(right) - intersection;
  return total > 0 ? intersection / total : 0;
}

double FilteredError(double previous, double current) {
  return kFollowFilterAlpha * current + (1 - kFollowFilterAlpha) * previous;
}

Task<Outcome> ObserveHuman(Execution& execution, bool v2) {
  using Clock = std::chrono::steady_clock;
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string device = arguments.at("gimbal_device_id").get<std::string>();
  const double confidence = arguments.value(
      "minimum_confidence", v2 ? kV2DefaultConfidence : kV1DefaultConfidence);
  const double reacquire_tilt =
      arguments.at("reacquire_tilt_rad").get<double>();
  const auto loss_timeout = std::chrono::milliseconds(
      arguments.value("target_loss_timeout_ms", 3000));
  const auto observation_hold =
      std::chrono::milliseconds(arguments.value("observation_hold_ms", 1500));
  const auto acquire_timeout_ms = arguments.value("acquire_timeout_ms", 60000);
  std::uint64_t fact_sequence = 0;
  const std::string uri = "robots/" + robot + "/detection/state";
  const std::string gimbal_uri =
      "robots/" + robot + "/devices/" + device + "/state";
  std::optional<ResourceSubscription> detection_subscription;
  if (auto opened = execution.OpenResourceSubscription(uri))
    detection_subscription.emplace(std::move(opened.value()));
  std::uint64_t last_publication_sequence = 0;
  bool read_subscription_baseline = false;
  const auto next_snapshot = [&]() -> std::optional<nlohmann::json> {
    if (!detection_subscription) {
      auto snapshot = execution.ReadResource(uri);
      return snapshot ? std::optional<nlohmann::json>(std::move(snapshot.value())) : std::nullopt;
    }
    if (!read_subscription_baseline) {
      read_subscription_baseline = true;
      auto history = execution.ReadResourceHistory(
          *detection_subscription, ResourceSelector{ResourceSelectorKind::kLatest});
      if (history && !history.value().publications.empty()) {
        auto publication = std::move(history.value().publications.front());
        last_publication_sequence = publication.sequence;
        return std::move(publication.value);
      }
    }
    auto waited = execution.WaitResource(*detection_subscription, last_publication_sequence, kPoll);
    if (!waited || waited.value().status != ResourceWaitStatus::kPublication ||
        !waited.value().publication)
      return std::nullopt;
    last_publication_sequence = waited.value().publication->sequence;
    return std::move(waited.value().publication->value);
  };
  std::uint64_t last_generation = 0;
  nlohmann::json candidate_bbox;
  std::string candidate_id;
  std::string candidate_registered_name;
  bool candidate_fallen_fallback = false;
  std::size_t stable_count = 0;
  std::uint64_t confirmation_generation = 0;
  nlohmann::json preceding_bbox;
  std::string preceding_id;
  std::string preceding_registered_name;
  bool preceding_fallen_fallback = false;
  std::size_t preceding_stable_count = 0;
  std::optional<Frame> stable_frame;
  std::optional<Target> stable_target;
  std::unordered_map<std::string, std::size_t> unknown_runs;
  std::unordered_map<std::string, std::size_t> preceding_unknown_runs;
  std::unordered_set<std::string> excluded_unknown_ids;
  std::uint64_t last_unknown_generation = 0;
  const auto update_unknown_faces = [&](const nlohmann::json& snapshot,
                                        std::uint64_t generation) {
    if (!v2) return;
    if (generation != last_unknown_generation) {
      preceding_unknown_runs = unknown_runs;
      last_unknown_generation = generation;
    }
    std::unordered_map<std::string, std::size_t> next;
    const auto detections = snapshot.find("detections");
    if (detections != snapshot.end() && detections->is_array()) {
      for (const auto& detection : *detections) {
        if (!detection.is_object() || detection.value("kind", "") != "human") continue;
        const std::string id = detection.value("id", "");
        if (id.empty()) continue;
        const auto attributes = detection.find("attributes");
        const bool explicitly_unknown = attributes != detection.end() &&
            attributes->is_object() && attributes->contains("name") &&
            attributes->at("name").is_string() &&
            attributes->at("name").get<std::string>() == "unknown";
        if (!explicitly_unknown) {
          excluded_unknown_ids.erase(id);
          continue;
        }
        const auto previous = preceding_unknown_runs.find(id);
        const std::size_t count = previous == preceding_unknown_runs.end() ? 1
                                                                           : previous->second + 1;
        next[id] = count;
        if (count >= kStableGenerations) excluded_unknown_ids.insert(id);
      }
    }
    unknown_runs = std::move(next);
  };
  std::string phase = "scan";
  std::string phase_before_pause = phase;
  std::optional<double> measured_pan_rate;
  std::optional<double> measured_tilt_rate;
  nlohmann::json last_public_state;
  const auto publish = [&] {
    nlohmann::json state{{"phase", phase}};
    if (stable_target && stable_target->id.size() <= 128) state["target_id"] = stable_target->id;
    if (measured_pan_rate && measured_tilt_rate) {
      const auto quantize = [](double value) { return std::round(value * 1000.0) / 1000.0; };
      state["pan_rate_rad_s"] = quantize(*measured_pan_rate);
      state["tilt_rate_rad_s"] = quantize(*measured_tilt_rate);
      state["speed_rad_s"] = quantize(std::hypot(*measured_pan_rate, *measured_tilt_rate));
    }
    if (state == last_public_state) return;
    last_public_state = state;
    (void)execution.PublishState(std::move(state));
  };
  const auto set_phase = [&](std::string next) {
    if (phase == next) return;
    phase = std::move(next);
    publish();
  };
  bool paused = false;
  const auto commands = [&] {
    while (true) {
      auto received = execution.ReceiveSignal(std::chrono::milliseconds(0));
      if (!received || !received.value()) return received ? Result<void>::Success()
                                                           : Result<void>::Failure(received.error());
      if (received.value()->name != "skill.command") continue;
      const std::string command = received.value()->payload.value("command", "");
      if (command == "pause" && !paused) {
        paused = true;
        phase_before_pause = phase;
        set_phase("pause");
      }
      if (command == "resume" && paused) {
        paused = false;
        set_phase(phase_before_pause);
      }
    }
  };

  publish();
  // The duration covers acquisition, tracking, and later scans.
  // Only a run that never found a person emits the absence fact.
  const std::optional<Clock::time_point> acquire_deadline =
      acquire_timeout_ms > 0
          ? std::optional<Clock::time_point>(Clock::now() +
                                             std::chrono::milliseconds(acquire_timeout_ms))
          : std::nullopt;
  const auto acquire_expired = [&] {
    return acquire_deadline && Clock::now() >= *acquire_deadline;
  };
  bool absent = false;
  std::optional<Target> reacquire_target;
  std::uint64_t acquisition_identity_boundary = 0;
  const auto confirm = [&](const nlohmann::json& snapshot, double initial_tilt) {
    auto frame = ParseFrame(snapshot);
    if (!frame || frame->generation < last_generation) return;
    if (frame->generation > last_generation) {
      last_generation = frame->generation;
    }
    if (!HumanDetectionReady(snapshot, frame->generation)) return;
    update_unknown_faces(snapshot, frame->generation);
    if (confirmation_generation != frame->generation) {
      confirmation_generation = frame->generation;
      preceding_bbox = candidate_bbox;
      preceding_id = candidate_id;
      preceding_registered_name = candidate_registered_name;
      preceding_fallen_fallback = candidate_fallen_fallback;
      preceding_stable_count = stable_count;
    }
    auto target = FindTarget(snapshot, *frame,
                             reacquire_target && reacquire_target->fallen_fallback
                                 ? reacquire_target->id : std::string{},
                             reacquire_target ? reacquire_target->registered_name : std::string{},
                             confidence, true, false,
                             acquisition_identity_boundary, initial_tilt, initial_tilt,
                             nullptr, v2, &excluded_unknown_ids);
    if (!target) {
      candidate_bbox = nullptr;
      candidate_id.clear();
      candidate_registered_name.clear();
      candidate_fallen_fallback = false;
      stable_count = 0;
      return;
    }
    const bool same_identity =
        target->fallen_fallback
            ? preceding_fallen_fallback && preceding_id == target->id
            : !preceding_fallen_fallback &&
                  preceding_registered_name == target->registered_name;
    if (!preceding_bbox.is_null() && same_identity &&
        BboxIou(preceding_bbox, target->bbox) >= kMinimumBboxIou) {
      stable_count = preceding_stable_count + 1;
    } else {
      stable_count = 1;
    }
    candidate_bbox = target->bbox;
    candidate_id = target->id;
    candidate_registered_name = target->registered_name;
    candidate_fallen_fallback = target->fallen_fallback;
    if (stable_count == kStableGenerations) {
      stable_frame = *frame;
      stable_target = *target;
    }
  };
  const auto drain = [&](PrimitiveOperation operation) {
    (void)execution.CancelPrimitive(operation);
    while (true) {
      auto waited = execution.WaitPrimitive(operation, kPoll);
      if (!waited || waited.value()) return;
    }
  };
  const auto drain_all = [&](std::vector<PrimitiveOperation>* operations) {
    for (auto& operation : *operations) drain(std::move(operation));
    operations->clear();
  };
  // Establish a generation boundary before moving, but acquire only during the bounded scan.
  while (last_generation == 0) {
    if (execution.cancelled()) co_return Outcome::Cancelled();
    if (acquire_expired()) {
      absent = true;
      break;
    }
    auto control = commands();
    if (!control) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    auto snapshot = next_snapshot();
    if (snapshot) {
      auto frame = ParseFrame(*snapshot);
      if (frame) last_generation = frame->generation;
    }
    co_await execution.DelayFor(kPoll);
  }
  acquisition_identity_boundary = last_generation;

  std::optional<double> scan_origin_pan;
  std::optional<double> scan_origin_tilt;
  while ((!scan_origin_pan || !scan_origin_tilt) && !absent) {
    if (execution.cancelled()) co_return Outcome::Cancelled();
    if (acquire_expired()) {
      absent = true;
      break;
    }
    auto control = commands();
    if (!control || paused) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    auto state = execution.ReadResource(gimbal_uri);
    if (state) {
      const auto pan = state.value().find("pan_rad");
      const auto tilt = state.value().find("tilt_rad");
      if (pan != state.value().end() && pan->is_number() &&
          tilt != state.value().end() && tilt->is_number()) {
        const double parsed_pan = pan->get<double>();
        const double parsed_tilt = tilt->get<double>();
        if (std::isfinite(parsed_pan) && std::isfinite(parsed_tilt)) {
          scan_origin_pan = parsed_pan;
          scan_origin_tilt = parsed_tilt;
        }
      }
    }
    if (!scan_origin_pan || !scan_origin_tilt) co_await execution.DelayFor(kPoll);
  }
  if (absent) {
    set_phase("absent");
    auto emitted = execution.EmitSignal(
        "state.fact", {{"key", "owner.found"}, {"value", false},
                       {"sequence", static_cast<std::int64_t>(++fact_sequence)}});
    if (!emitted) co_return Outcome::Failed("fact.emit_failed", emitted.error().message);
    co_return Outcome::Completed({{"found", false}});
  }
  const double initial_tilt = *scan_origin_tilt;

  const auto move_to = [&](double pan, double tilt) -> Task<bool> {
    while (!stable_target && !absent) {
      std::optional<PrimitiveOperation> motion;
      while (!motion) {
        if (execution.cancelled()) co_return false;
        if (acquire_expired()) {
          absent = true;
          co_return false;
        }
        auto control = commands();
        if (!control || paused) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        auto started = execution.StartPrimitive(
            {"gimbal.motion", "1.0.0"}, device,
            {{"reference", "absolute"}, {"pan_rad", pan}, {"tilt_rad", tilt},
             {"speed_rad_s", 0.1}, {"stream", false}});
        if (started) motion = started.value();
        else co_await execution.DelayFor(kPoll);
      }
      while (!stable_target) {
        if (execution.cancelled()) {
          drain(*motion);
          co_return false;
        }
        if (acquire_expired()) {
          drain(*motion);
          absent = true;
          co_return false;
        }
        auto control = commands();
        if (!control) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        if (paused) {
          drain(*motion);
          break;
        }
        auto snapshot = next_snapshot();
        if (snapshot) confirm(*snapshot, initial_tilt);
        if (stable_target) {
          drain(*motion);
          co_return false;
        }
        auto moved = execution.WaitPrimitive(*motion, std::chrono::milliseconds(0));
        if (!moved || moved.value()) co_return true;
        co_await execution.DelayFor(kPoll);
      }
    }
    co_return false;
  };

  const auto initial_observation_deadline = Clock::now() + observation_hold;
  while (!stable_target && !absent && Clock::now() < initial_observation_deadline) {
    if (execution.cancelled()) co_return Outcome::Cancelled();
    if (acquire_expired()) {
      absent = true;
      break;
    }
    auto control = commands();
    if (!control || paused) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    auto snapshot = next_snapshot();
    if (snapshot) confirm(*snapshot, initial_tilt);
    if (!stable_target) co_await execution.DelayFor(kPoll);
  }

  if (!stable_target && !absent) {
    auto downward_motion = move_to(
        *scan_origin_pan, initial_tilt - kInitialDownwardTiltDeltaRad);
    const bool reached_downward_pose = co_await downward_motion;
    if (reached_downward_pose) {
      const auto downward_observation_deadline = Clock::now() + kInitialDownwardObservationHold;
      while (!stable_target && !absent && Clock::now() < downward_observation_deadline) {
        if (execution.cancelled()) co_return Outcome::Cancelled();
        if (acquire_expired()) {
          absent = true;
          break;
        }
        auto control = commands();
        if (!control || paused) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        auto snapshot = next_snapshot();
        if (snapshot) confirm(*snapshot, initial_tilt);
        if (!stable_target) co_await execution.DelayFor(kPoll);
      }
      if (!stable_target && !absent) {
        auto return_motion = move_to(*scan_origin_pan, initial_tilt);
        (void)co_await return_motion;
      }
    }
  }

  const double pi = 3.14159265358979323846;
  const double scan_origin = scan_origin_pan.value_or(0.0);
  const double scan_targets[] = {scan_origin - pi / 4, scan_origin, scan_origin + pi / 4,
                                 scan_origin};
  std::size_t target_index = 0;
  const auto scan_waypoints = [&]() -> Task<bool> {
    while (!absent && !stable_target) {
      const double target_pan = scan_targets[target_index];
      std::optional<PrimitiveOperation> motion;
      while (!motion) {
        if (execution.cancelled()) co_return false;
        if (acquire_expired()) {
          absent = true;
          break;
        }
        auto control = commands();
        if (!control || paused) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        auto started = execution.StartPrimitive(
            {"gimbal.motion", "1.0.0"}, device,
            {{"reference", "absolute"}, {"pan_rad", target_pan}, {"tilt_rad", *scan_origin_tilt},
             {"speed_rad_s", 0.1}, {"stream", false}});
        if (started) {
          motion = started.value();
        } else {
          co_await execution.DelayFor(kPoll);
        }
      }
      if (absent) break;
      while (!stable_target) {
        if (execution.cancelled()) {
          drain(*motion);
          co_return false;
        }
        if (acquire_expired()) {
          drain(*motion);
          absent = true;
          break;
        }
        auto control = commands();
        if (!control) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        if (paused) {
          drain(*motion);
          motion.reset();
          break;
        }
        if (!paused) {
          auto snapshot = next_snapshot();
          if (snapshot) confirm(*snapshot, initial_tilt);
        }
        if (stable_target) {
          drain(*motion);
          break;
        }
        auto moved = execution.WaitPrimitive(*motion, std::chrono::milliseconds(0));
        if (!moved) break;
        if (moved.value()) {
          break;
        }
        co_await execution.DelayFor(kPoll);
      }
      if (stable_target) break;
      if (absent) break;
      if (!motion) {
        continue;
      }
      const auto observation_deadline = Clock::now() + observation_hold;
      while (!stable_target && !absent && Clock::now() < observation_deadline) {
        if (execution.cancelled()) co_return false;
        if (acquire_expired()) {
          absent = true;
          break;
        }
        auto control = commands();
        if (!control || paused) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        auto snapshot = next_snapshot();
        if (snapshot) confirm(*snapshot, initial_tilt);
        if (!stable_target) co_await execution.DelayFor(kPoll);
      }
      target_index = (target_index + 1) % 4;
    }
    co_return stable_target.has_value();
  };
  auto initial_scan = scan_waypoints();
  const bool initially_found = co_await initial_scan;
  if (!initially_found && execution.cancelled()) co_return Outcome::Cancelled();

  if (absent) {
    set_phase("absent");
    if (scan_origin_pan && scan_origin_tilt) {
      std::optional<PrimitiveOperation> leveling;
      while (!leveling) {
        if (execution.cancelled()) co_return Outcome::Cancelled();
        auto started = execution.StartPrimitive(
            {"gimbal.motion", "1.0.0"}, device,
            {{"reference", "absolute"}, {"pan_rad", *scan_origin_pan},
             {"tilt_rad", reacquire_tilt}, {"speed_rad_s", 0.1}, {"stream", false}});
        if (started) {
          leveling = started.value();
        } else {
          co_await execution.DelayFor(kPoll);
        }
      }
      while (true) {
        if (execution.cancelled()) {
          drain(*leveling);
          co_return Outcome::Cancelled();
        }
        auto control = commands();
        if (!control) {
          co_await execution.DelayFor(kPoll);
          continue;
        }
        if (paused) {
          drain(*leveling);
          leveling.reset();
          break;
        }
        auto leveled = execution.WaitPrimitive(*leveling, std::chrono::milliseconds(0));
        if (!leveled) {
          drain(*leveling);
          break;
        }
        if (leveled.value()) break;
        co_await execution.DelayFor(kPoll);
      }
    }
    auto emitted = execution.EmitSignal(
        "state.fact", {{"key", "owner.found"},
                       {"value", false},
                       {"sequence", static_cast<std::int64_t>(++fact_sequence)}});
    if (!emitted) co_return Outcome::Failed("fact.emit_failed", emitted.error().message);
    co_return Outcome::Completed({{"found", false}});
  }

  set_phase("confirm");
  if (execution.cancelled()) co_return Outcome::Cancelled();
  set_phase("track");
  publish();
  std::optional<Clock::time_point> lost_since;
  std::vector<PrimitiveOperation> corrections;
  bool target_visible = true;
  double filtered_horizontal_error = stable_target->horizontal_error;
  double filtered_vertical_error = stable_target->vertical_error;
  double tracked_frame_width = stable_frame->width;
  double tracked_frame_height = stable_frame->height;
  Frame tracked_frame = *stable_frame;
  detail::CenteredState centered_state{
      std::abs(filtered_horizontal_error) <= kHorizontalDeadband &&
          std::abs(filtered_vertical_error) <= kVerticalDeadband,
      0};
  bool reset_tracking_filter = false;
  bool correction_pending = true;
  std::uint64_t last_correction_generation = 0;
  std::uint64_t last_tracking_sample_generation = 0;
  auto next_speed_sample = Clock::now();
  while (true) {
    if (Clock::now() >= next_speed_sample) {
      next_speed_sample = Clock::now() + std::chrono::milliseconds(100);
      auto gimbal_state = execution.ReadResource(gimbal_uri);
      bool valid_speed = false;
      if (gimbal_state) {
        const auto pan_rate = gimbal_state.value().find("pan_rate_rad_s");
        const auto tilt_rate = gimbal_state.value().find("tilt_rate_rad_s");
        if (pan_rate != gimbal_state.value().end() && pan_rate->is_number() &&
            tilt_rate != gimbal_state.value().end() && tilt_rate->is_number()) {
          const double pan_value = pan_rate->get<double>();
          const double tilt_value = tilt_rate->get<double>();
          if (std::isfinite(pan_value) && std::isfinite(tilt_value) &&
              std::abs(pan_value) <= 100.0 && std::abs(tilt_value) <= 100.0) {
            measured_pan_rate = pan_value;
            measured_tilt_rate = tilt_value;
            valid_speed = true;
          }
        }
      }
      if (!valid_speed) {
        measured_pan_rate.reset();
        measured_tilt_rate.reset();
      }
      publish();
    }
    if (execution.cancelled()) {
      drain_all(&corrections);
      publish();
      co_return Outcome::Cancelled();
    }
    if (acquire_expired()) {
      drain_all(&corrections);
      co_return Outcome::Completed({{"found", true}});
    }
    auto control = commands();
    if (!control) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    if (paused) {
      drain_all(&corrections);
      lost_since.reset();
      centered_state = {};
      reset_tracking_filter = true;
      correction_pending = false;
      co_await execution.DelayFor(kPoll);
      continue;
    }
    for (auto operation = corrections.begin(); operation != corrections.end();) {
      auto moved = execution.WaitPrimitive(*operation, std::chrono::milliseconds(0));
      if (!moved) {
        drain(*operation);
        operation = corrections.erase(operation);
      } else if (moved.value()) {
        operation = corrections.erase(operation);
      } else {
        ++operation;
      }
    }
    auto snapshot = next_snapshot();
    if (snapshot) {
      auto frame = ParseFrame(*snapshot);
      if (frame && frame->generation > last_generation) last_generation = frame->generation;
      if (frame && frame->generation == last_generation &&
          HumanDetectionReady(*snapshot, frame->generation)) {
        update_unknown_faces(*snapshot, frame->generation);
        auto target = FindTarget(*snapshot, *frame, stable_target->id,
                                 stable_target->registered_name, confidence, false, true,
                                 std::nullopt, std::nullopt, std::nullopt,
                                 nullptr, v2, &excluded_unknown_ids);
        if (!target && (!v2 || !stable_target->registered_name.empty())) {
          target = FindTarget(*snapshot, *frame, {}, stable_target->registered_name, confidence,
                              false, true, std::nullopt, std::nullopt, std::nullopt,
                              &stable_target->bbox, v2, &excluded_unknown_ids);
        }
        if (!target) {
          drain_all(&corrections);
          target_visible = false;
          centered_state = {};
          reset_tracking_filter = true;
          correction_pending = false;
          if (!lost_since) lost_since = Clock::now();
          if (Clock::now() - *lost_since >= loss_timeout) {
            reacquire_target = stable_target;
            stable_target.reset();
            acquisition_identity_boundary = last_generation;
            candidate_bbox = nullptr;
            candidate_id.clear();
            candidate_registered_name.clear();
            candidate_fallen_fallback = false;
            stable_count = 0;
            confirmation_generation = 0;
            set_phase("scan");
            auto next_scan = scan_waypoints();
            const bool reacquired = co_await next_scan;
            if (!reacquired && execution.cancelled()) co_return Outcome::Cancelled();
            if (!reacquired) co_return Outcome::Completed({{"found", true}});
            reacquire_target.reset();
            filtered_horizontal_error = stable_target->horizontal_error;
            filtered_vertical_error = stable_target->vertical_error;
            tracked_frame_width = stable_frame->width;
            tracked_frame_height = stable_frame->height;
            tracked_frame = *stable_frame;
            centered_state = {std::abs(filtered_horizontal_error) <= kHorizontalDeadband &&
                                  std::abs(filtered_vertical_error) <= kVerticalDeadband,
                              0};
            reset_tracking_filter = false;
            target_visible = true;
            lost_since.reset();
            correction_pending = true;
            set_phase("track");
            continue;
          }
        } else {
          stable_target->id = target->id;
          if (stable_target->registered_name.empty() && !target->registered_name.empty()) {
            stable_target->registered_name = target->registered_name;
            stable_target->fallen_fallback = false;
          }
          stable_target->bbox = target->bbox;
          stable_target->horizontal_error = target->horizontal_error;
          stable_target->vertical_error = target->vertical_error;
          publish();
          target_visible = true;
          lost_since.reset();
          if (frame->generation == last_tracking_sample_generation) continue;
          last_tracking_sample_generation = frame->generation;
          if (reset_tracking_filter) {
            filtered_horizontal_error = target->horizontal_error;
            filtered_vertical_error = target->vertical_error;
            reset_tracking_filter = false;
          } else {
            filtered_horizontal_error =
                FilteredError(filtered_horizontal_error, target->horizontal_error);
            filtered_vertical_error =
                FilteredError(filtered_vertical_error, target->vertical_error);
          }
          tracked_frame_width = frame->width;
          tracked_frame_height = frame->height;
          tracked_frame = *frame;
          const bool was_centered = centered_state.centered;
          centered_state = detail::UpdateCenteredState(
              centered_state, filtered_horizontal_error, filtered_vertical_error,
              kHorizontalDeadband, kVerticalDeadband, kHorizontalExitDeadband,
              kVerticalExitDeadband);
          correction_pending = frame->generation != last_correction_generation &&
                               (!centered_state.centered || !was_centered);
        }
      }
    }
    if (!target_visible || !correction_pending ||
        tracked_frame.generation == last_correction_generation) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    const double vertical_fov = detail::VerticalFovRad(
        detail::kHorizontalFovRad, tracked_frame_width, tracked_frame_height);
    const auto pan_correction = detail::BoundedFollowCorrection(
        filtered_horizontal_error, detail::kHorizontalFovRad, kHorizontalDeadband,
        kNearCenterLimit, kNearCenterPanCapRad);
    const auto tilt_correction = detail::BoundedFollowCorrection(
        filtered_vertical_error, vertical_fov, kVerticalDeadband, kNearCenterLimit,
        kNearCenterTiltCapRad);
    const double pan = centered_state.centered ? 0.0 : -pan_correction.effective_angle_rad;
    const double requested_tilt =
        centered_state.centered ? 0.0 : -tilt_correction.effective_angle_rad;
    auto gimbal_state = execution.ReadResource(gimbal_uri);
    if (!gimbal_state) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    const auto current_pan_field = gimbal_state.value().find("pan_rad");
    const auto current_tilt_field = gimbal_state.value().find("tilt_rad");
    if (current_pan_field == gimbal_state.value().end() || !current_pan_field->is_number() ||
        current_tilt_field == gimbal_state.value().end() || !current_tilt_field->is_number()) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    const double current_pan = current_pan_field->get<double>();
    const double current_tilt = current_tilt_field->get<double>();
    if (!std::isfinite(current_pan) || !std::isfinite(current_tilt)) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    const bool capture_aligned = !centered_state.centered && tracked_frame.gimbal_pan_rad &&
                                 tracked_frame.gimbal_tilt_rad;
    const double reference_pan = capture_aligned ? *tracked_frame.gimbal_pan_rad : current_pan;
    const double reference_tilt = capture_aligned ? *tracked_frame.gimbal_tilt_rad : current_tilt;
    const double target_pan = reference_pan + pan;
    const double target_tilt = std::clamp(reference_tilt + requested_tilt,
                                          initial_tilt - kMaximumTiltDelta, initial_tilt);
    auto started = execution.StartPrimitive(
        {"gimbal.motion", "1.0.0"}, device,
        {{"reference", "absolute"}, {"pan_rad", target_pan}, {"tilt_rad", target_tilt},
         {"speed_rad_s", kFollowSpeedRadPerSecond}, {"stream", true}});
    if (!started) {
      co_await execution.DelayFor(kPoll);
      continue;
    }
    correction_pending = false;
    last_correction_generation = tracked_frame.generation;
    corrections.push_back(started.value());
    co_await execution.DelayFor(kPoll);
  }
}
}  // namespace

void RegisterObserveHuman(Registry& registry) {
  Definition skill;
  skill.metadata.name = "observe_human";
  skill.metadata.version = "1.0.0";
  skill.metadata.description = "Tracks one stable human with the gimbal.";
  skill.metadata.library = "island.observe_human";
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  skill.metadata.parameters_schema = {
      {"type", "object"},
      {"properties", {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
                       {"gimbal_device_id", {{"type", "string"}, {"minLength", 1}}},
                       {"minimum_confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                       {"reacquire_tilt_rad",
                        {{"type", "number"}, {"minimum", -2.233}, {"maximum", 2.233}}},
                        {"target_loss_timeout_ms", {{"type", "integer"}, {"minimum", 1}}},
                        {"observation_hold_ms",
                         {{"type", "integer"}, {"minimum", 0}, {"maximum", 10000}}},
                        {"acquire_timeout_ms", {{"type", "integer"}, {"minimum", 1}}}}},
      {"required", {"robot_name", "gimbal_device_id", "reacquire_tilt_rad"}}, {"additionalProperties", false}};
  skill.metadata.returns_schema = {
      {"type", "object"},
      {"properties", {{"found", {{"type", "boolean"}}}}},
      {"required", {"found"}},
      {"additionalProperties", false}};
  skill.metadata.execution_state_schema = {
      {"type", "object"},
      {"properties",
       {{"phase", {{"type", "string"},
                    {"enum", {"scan", "confirm", "track", "reacquire", "pause", "absent"}}}},
        {"target_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
        {"pan_rate_rad_s", {{"type", "number"}, {"minimum", -100}, {"maximum", 100}}},
        {"tilt_rate_rad_s", {{"type", "number"}, {"minimum", -100}, {"maximum", 100}}},
        {"speed_rad_s", {{"type", "number"}, {"minimum", 0}, {"maximum", 100}}}}},
      {"required", {"phase"}},
      {"maxProperties", 5},
      {"additionalProperties", false}};
  skill.metadata.primitives.push_back({"gimbal.motion", "1.0.0"});
  skill.metadata.resources.push_back(
      {"robots/{robot_name}/detection/state", "Retained semantic frames used for tracking."});
  skill.metadata.resources.push_back(
      {"robots/{robot_name}/devices/{gimbal_device_id}/state",
       "Measured gimbal position and angular velocity used for tracking diagnostics."});
  skill.metadata.claim_templates.push_back(
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/gimbal_device_id"});
  skill.metadata.accepted_signals.push_back(
      {"skill.command", {{"type", "object"},
                          {"properties", {{"command", {{"enum", {"pause", "resume"}}}}}},
                         {"required", {"command"}}, {"additionalProperties", false}}});
  skill.metadata.emitted_signals.push_back(
      {"state.fact", {{"type", "object"},
                       {"properties", {{"key", {{"const", "owner.found"}}},
                                       {"value", {{"const", false}}},
                                       {"sequence", {{"type", "integer"}, {"minimum", 1}}}}},
                       {"required", {"key", "value", "sequence"}},
                       {"additionalProperties", false}}});
  skill.metadata.supports_signals = true;
  skill.metadata.reentrant = true;
  Definition legacy = skill;
  legacy.metadata.name = "observe_human_legacy";
  legacy.handler = [](Execution& execution) { return ObserveHuman(execution, false); };
  skill.metadata.description = "Tracks one selected human using face and body evidence.";
  skill.handler = [](Execution& execution) { return ObserveHuman(execution, true); };
  registry.Register(std::move(legacy));
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native

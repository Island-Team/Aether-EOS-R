#include "share/skills/native/common/assist_support.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include "share/skills/native/common/navigation_trash_support.h"

namespace island::skills::assist {
namespace {

void EmitEvent(Execution& execution, const ControlState& control, std::string_view phase,
               std::string_view state, double progress) {
  Json payload{
      {"skill", control.skill}, {"phase", phase}, {"state", state}, {"progress", progress}};
  if (control.source_execution_id) payload["source_execution_id"] = *control.source_execution_id;
  if (control.correlation_id) payload["correlation_id"] = *control.correlation_id;
  (void)execution.EmitSignal("skill.event", payload);
}

void ApplyCommand(Execution& execution, const eos::runtime::skills::Signal& signal,
                  ControlState* control) {
  if (signal.name != "skill.command") return;
  const std::string command = signal.payload.value("command", "");
  const bool was_paused = control->paused;
  if (command == "pause") control->paused = true;
  if (command == "resume") control->paused = false;
  if (signal.payload.contains("source_execution_id")) {
    control->source_execution_id = signal.payload.at("source_execution_id").get<std::string>();
  }
  if (signal.payload.contains("correlation_id")) {
    control->correlation_id = signal.payload.at("correlation_id").get<std::string>();
  }
  if (was_paused == control->paused) return;
  EmitEvent(execution, *control, "control", control->paused ? "paused" : "resumed", 0.0);
  (void)execution.Checkpoint({control->paused ? "control.paused" : "control.resumed", {}, {}, {}});
}

PrimitiveResult ConvertPrimitiveResult(const primitive::Result& result) {
  switch (result.status) {
    case primitive::Status::kSucceeded: return {PrimitiveStatus::kSucceeded, result.message};
    case primitive::Status::kRejected: return {PrimitiveStatus::kRejected, result.message};
    case primitive::Status::kCancelled: return {PrimitiveStatus::kCancelled, result.message};
    case primitive::Status::kTimedOut: return {PrimitiveStatus::kTimedOut, result.message};
    case primitive::Status::kOutcomeUnknown:
      return {PrimitiveStatus::kOutcomeUnknown, result.message};
    case primitive::Status::kFailed: return {PrimitiveStatus::kFailed, result.message};
  }
  return {PrimitiveStatus::kFailed, "primitive returned an unknown status"};
}

bool ContainsAny(std::string_view text, std::initializer_list<std::string_view> values) {
  return std::any_of(values.begin(), values.end(), [text](std::string_view value) {
    return text.find(value) != std::string_view::npos;
  });
}

}  // namespace

Task<bool> WaitUntilRunnable(Execution& execution, ControlState* control, std::string* error) {
  while (!execution.cancelled()) {
    const auto received = execution.ReceiveSignal(std::chrono::milliseconds(0));
    if (!received) {
      control->signal_error = SafeCapabilityError(received.error());
      if (error != nullptr) *error = control->signal_error;
      co_return false;
    }
    if (received.value()) {
      ApplyCommand(execution, *received.value(), control);
      continue;
    }
    if (!control->paused) co_return true;
    co_await execution.DelayFor(kPollInterval);
  }
  co_return false;
}

void Checkpoint(Execution& execution, const ControlState& control, std::string_view name,
                double progress, std::string_view state) {
  (void)execution.Checkpoint({std::string(name), {}, progress, {}});
  EmitEvent(execution, control, name, state, progress);
}

const char* SafeCapabilityError(const eos::runtime::skills::builtin::Error&) {
  return "capability call failed";
}

bool ReadJson(Execution& execution, const std::string& uri, Json* value, std::string* error) {
  auto response = execution.ReadResource(uri);
  if (!response) {
    if (error != nullptr) error->assign(SafeCapabilityError(response.error()));
    return false;
  }
  *value = std::move(response.value());
  return true;
}

bool ReadDetection(Execution& execution, const std::string& uri, Snapshot* snapshot,
                   std::string* error) {
  Json value;
  if (!ReadJson(execution, uri, &value, error)) return false;
  if (!value.contains("source_sequence") || !value.at("source_sequence").is_number_unsigned()) {
    if (error != nullptr) *error = "detection state was invalid";
    return false;
  }
  snapshot->source_sequence = value.at("source_sequence").get<std::uint64_t>();
  snapshot->value = std::move(value);
  return true;
}

bool ReadJoint4(Execution& execution, const std::string& uri, double* joint4_rad,
                std::string* error) {
  Json value;
  if (!ReadJson(execution, uri, &value, error)) return false;
  const auto joints = value.find("joint_positions_rad");
  const auto valid = value.find("joint_valid");
  if (joints == value.end() || !joints->is_array() || joints->size() <= 3 || valid == value.end() ||
      !valid->is_array() || valid->size() <= 3 || !valid->at(3).is_boolean() ||
      !valid->at(3).get<bool>() || !joints->at(3).is_number()) {
    if (error != nullptr) *error = "arm state was invalid";
    return false;
  }
  *joint4_rad = joints->at(3).get<double>();
  if (!std::isfinite(*joint4_rad)) {
    if (error != nullptr) *error = "arm state was invalid";
    return false;
  }
  return true;
}

Task<bool> WaitForDetectionAfter(Execution& execution, ControlState* control, const std::string& uri,
                                 std::uint64_t previous_sequence, Clock::time_point deadline,
                                 Snapshot* fresh, std::string* error) {
  while (Clock::now() < deadline) {
    auto runnable = WaitUntilRunnable(execution, control, error);
    if (!co_await runnable) co_return false;
    Snapshot current;
    if (!ReadDetection(execution, uri, &current, error)) co_return false;
    if (current.source_sequence > previous_sequence) {
      *fresh = std::move(current);
      co_return true;
    }
    co_await execution.DelayFor(kPollInterval);
  }
  co_return false;
}

const Json* FindHuman(const Json& snapshot, const std::string& expected_id) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return nullptr;
  for (const auto& detection : *detections) {
    if (!detection.is_object() || detection.value("kind", "") != "human") continue;
    if (detection.value("id", "") == expected_id) return &detection;
  }
  return nullptr;
}

bool MatchesAction(const Json& detection, std::string_view action,
                   std::string_view legacy_activity) {
  const auto attributes = detection.find("attributes");
  if (attributes == detection.end() || !attributes->is_object()) return false;
  const auto canonical = attributes->find("action");
  if (canonical != attributes->end() && canonical->is_string())
    return canonical->get<std::string_view>() == action;
  const auto legacy = attributes->find("activity");
  return legacy != attributes->end() && legacy->is_string() &&
         legacy->get<std::string_view>() == legacy_activity;
}

bool MatchesPose(const Json& detection, std::string_view pose,
                 std::string_view legacy_activity) {
  const auto attributes = detection.find("attributes");
  if (attributes == detection.end() || !attributes->is_object()) return false;
  const auto canonical = attributes->find("pose");
  if (canonical != attributes->end() && canonical->is_string())
    return canonical->get<std::string_view>() == pose;
  const auto legacy = attributes->find("activity");
  return legacy != attributes->end() && legacy->is_string() &&
         legacy->get<std::string_view>() == legacy_activity;
}

bool MatchesCondition(const Json& detection, HumanCondition condition) {
  if (condition == HumanCondition::kFallen)
    return MatchesAction(detection, "fallen", "fallen");
  if (condition == HumanCondition::kSitting)
    return MatchesPose(detection, "sit", "sitting");
  return true;
}

bool ReadNumber(const Json& object, const char* key, double* output) {
  if (!object.contains(key) || !object.at(key).is_number()) return false;
  *output = object.at(key).get<double>();
  return std::isfinite(*output);
}

std::optional<bool> CenteredInImage(const Json& snapshot, const Json& human,
                                    double deadband_fraction) {
  double width_px = 0.0;
  double height_px = 0.0;
  double confidence = 0.0;
  if (!ReadNumber(snapshot, "image_width_px", &width_px) || width_px <= 0.0 ||
      !ReadNumber(snapshot, "image_height_px", &height_px) || height_px <= 0.0 ||
      !ReadNumber(human, "confidence", &confidence) || confidence < kMinimumDetectionConfidence) {
    return std::nullopt;
  }
  const auto bbox = human.find("bbox");
  if (bbox == human.end() || !bbox->is_array() || bbox->size() != 4) return std::nullopt;
  double bounds[4]{};
  for (std::size_t index = 0; index < 4; ++index) {
    if (!bbox->at(index).is_number()) return std::nullopt;
    bounds[index] = bbox->at(index).get<double>();
    if (!std::isfinite(bounds[index])) return std::nullopt;
  }
  if (bounds[0] < 0.0 || bounds[1] < 0.0 || bounds[2] > width_px || bounds[3] > height_px ||
      bounds[0] >= bounds[2] || bounds[1] >= bounds[3]) {
    return std::nullopt;
  }
  const double image_error = (bounds[0] + bounds[2] - width_px) / width_px;
  return std::abs(image_error) <= deadband_fraction;
}

Task<PrimitiveResult> CallPrimitive(Execution& execution, const char* primitive_name,
                                     std::string version, std::string device_id,
                                     Json arguments, ControlState* control) {
  auto task = primitive::Run(
      execution, {primitive_name, version}, device_id, arguments, [&]() -> eos::runtime::skills::builtin::Result<bool> {
        const auto received = execution.ReceiveSignal(std::chrono::milliseconds(0));
        if (!received) {
          control->signal_error = SafeCapabilityError(received.error());
          return eos::runtime::skills::builtin::Result<bool>::Failure(received.error());
        }
        if (received.value()) ApplyCommand(execution, *received.value(), control);
        return eos::runtime::skills::builtin::Result<bool>::Success(!control->paused);
      });
  co_return ConvertPrimitiveResult(co_await task);
}

Task<PrimitiveResult> NavigateWhileObserving(Execution& execution, ControlState* control,
                                          std::string base_device_id, Json arguments,
                                          std::string detection_uri,
                                          std::string target_id, Snapshot* latest,
                                           HumanCondition condition, std::string expected_name,
                                            std::string required_action,
                                            std::string* reassociated_target_id,
                                            bool cancel_on_invalid_target,
                                            std::string head_detection_uri,
                                            std::string speaker_device_id) {
  bool target_lost = false;
  bool target_observed = true;
  auto poll_owner = [&]() -> eos::runtime::skills::builtin::Result<bool> {
    const auto received = execution.ReceiveSignal(std::chrono::milliseconds(0));
    if (!received) {
      control->signal_error = SafeCapabilityError(received.error());
      return eos::runtime::skills::builtin::Result<bool>::Failure(received.error());
    }
    if (received.value()) ApplyCommand(execution, *received.value(), control);
    if (control->paused) return eos::runtime::skills::builtin::Result<bool>::Success(false);
    Snapshot fresh;
    std::string error;
    if (!ReadDetection(execution, detection_uri, &fresh, &error)) {
      if (!cancel_on_invalid_target) {
        if (target_observed)
          Checkpoint(execution, *control, "navigation.target_unobserved", 0.75);
        target_observed = false;
        return eos::runtime::skills::builtin::Result<bool>::Success(true);
      }
      return eos::runtime::skills::builtin::Result<bool>::Failure(
          {"capability.failed", "navigation monitoring: " + error});
    }
    if (fresh.source_sequence <= latest->source_sequence)
      return eos::runtime::skills::builtin::Result<bool>::Success(true);
    *latest = std::move(fresh);
    const Json* target = FindHuman(latest->value, target_id);
    if (target == nullptr && !expected_name.empty()) {
      const Json* reassociated = nullptr;
      const auto detections = latest->value.find("detections");
      if (detections != latest->value.end() && detections->is_array()) {
        for (const auto& candidate : *detections) {
          if (!candidate.is_object() || candidate.value("kind", "") != "human" ||
              candidate.value("id", "").empty())
            continue;
          const Json attributes = candidate.value("attributes", Json::object());
          const auto name = attributes.find("name");
          double candidate_distance_m = 0.0;
          if (!attributes.is_object() || name == attributes.end() || !name->is_string() ||
              name->get<std::string_view>() != expected_name ||
              !MatchesCondition(candidate, condition) ||
              (!required_action.empty() &&
               !MatchesAction(candidate, required_action, required_action)) ||
              !ReadNumber(candidate, "distance_m", &candidate_distance_m) ||
              candidate_distance_m < 0.0)
            continue;
          if (reassociated != nullptr) {
            reassociated = nullptr;
            break;
          }
          reassociated = &candidate;
        }
      }
      if (reassociated != nullptr) {
        target = reassociated;
        target_id = target->value("id", "");
        if (reassociated_target_id != nullptr) *reassociated_target_id = target_id;
      }
    }
    double distance_m = 0.0;
    const Json attributes = target == nullptr ? Json::object()
                                               : target->value("attributes", Json::object());
    const auto name = attributes.find("name");
    const bool identity_matches =
        expected_name.empty() ||
        (attributes.is_object() && name != attributes.end() && name->is_string() &&
         name->get<std::string_view>() == expected_name);
    const bool action_matches =
        required_action.empty() ||
        (target != nullptr && MatchesAction(*target, required_action, required_action));
    if (target == nullptr || !MatchesCondition(*target, condition) || !identity_matches ||
        !action_matches ||
        !ReadNumber(*target, "distance_m", &distance_m) || distance_m < 0.0) {
      if (!cancel_on_invalid_target) {
        if (target_observed)
          Checkpoint(execution, *control, "navigation.target_unobserved", 0.75);
        target_observed = false;
        return eos::runtime::skills::builtin::Result<bool>::Success(true);
      }
      target_lost = true;
      return eos::runtime::skills::builtin::Result<bool>::Failure(
          {"target.lost", "navigation cancelled because the tracked target is no longer valid"});
    }
    if (!target_observed)
      Checkpoint(execution, *control, "navigation.target_observed", 0.75);
    target_observed = true;
    return eos::runtime::skills::builtin::Result<bool>::Success(true);
  };
  if (head_detection_uri.empty()) {
    auto task = primitive::Run(execution, {"base.navigate", "1.0.0"}, base_device_id, arguments,
                               poll_owner);
    auto result = ConvertPrimitiveResult(co_await task);
    if (target_lost && result.status == PrimitiveStatus::kFailed)
      result.status = PrimitiveStatus::kTargetLost;
    co_return result;
  }

  auto started = execution.StartPrimitive({"base.navigate", "1.0.0"}, base_device_id, arguments);
  if (!started) co_return PrimitiveResult{PrimitiveStatus::kFailed, started.error().message};
  PrimitiveOperation navigation = std::move(started.value());
  navigation_trash::Detector trash_detector;
  while (!execution.cancelled()) {
    auto owner = poll_owner();
    if (!owner) {
      (void)execution.CancelPrimitive(navigation);
      co_return PrimitiveResult{target_lost ? PrimitiveStatus::kTargetLost : PrimitiveStatus::kFailed,
                                target_lost ? "tracked target was lost" : "navigation monitoring failed"};
    }
    auto nav_wait = execution.WaitPrimitive(navigation, std::chrono::milliseconds(0));
    if (!nav_wait) {
      (void)execution.CancelPrimitive(navigation);
      co_return PrimitiveResult{PrimitiveStatus::kFailed, nav_wait.error().message};
    }
    if (nav_wait.value()) {
      co_return ConvertPrimitiveResult(primitive::ParseAcknowledgement(*nav_wait.value()));
    }

    Json head;
    std::string head_error;
    if (!ReadJson(execution, head_detection_uri, &head, &head_error)) {
      (void)execution.CancelPrimitive(navigation);
      co_return PrimitiveResult{PrimitiveStatus::kFailed, "trash monitoring: " + head_error};
    }
    const auto found = trash_detector.Observe(head);
    if (!found) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }

    Checkpoint(execution, *control, "trash.found", 0.55);
    auto speech = execution.StartPrimitive(
        {"speaker.play", "1.0.0"}, speaker_device_id,
        {{"uri", navigation_trash::kTrash[*found].uri}});
    const auto cancelled = execution.CancelNavigationCommand(navigation.command_id, base_device_id);
    const bool cancellation_confirmed =
        cancelled && cancelled.value() == eos::runtime::skills::builtin::NavigationCancelStatus::kCancelled;
    bool navigation_terminal = false;
    bool speech_terminal = !speech;
    while (!execution.cancelled() && (!navigation_terminal || !speech_terminal)) {
      auto monitoring = poll_owner();
      if (!monitoring) {
        if (speech) (void)execution.CancelPrimitive(speech.value());
        (void)execution.CancelPrimitive(navigation);
        co_return PrimitiveResult{target_lost ? PrimitiveStatus::kTargetLost : PrimitiveStatus::kFailed,
                                  "navigation monitoring failed"};
      }
      if (!navigation_terminal) {
        auto waited = execution.WaitPrimitive(navigation, std::chrono::milliseconds(0));
        if (!waited) {
          if (speech) (void)execution.CancelPrimitive(speech.value());
          (void)execution.CancelPrimitive(navigation);
          co_return PrimitiveResult{PrimitiveStatus::kFailed, waited.error().message};
        }
        navigation_terminal = waited.value().has_value();
      }
      if (!speech_terminal && speech) {
        auto waited = execution.WaitPrimitive(speech.value(), std::chrono::milliseconds(0));
        if (!waited) {
          (void)execution.CancelPrimitive(speech.value());
          if (!navigation_terminal) (void)execution.CancelPrimitive(navigation);
          co_return PrimitiveResult{PrimitiveStatus::kFailed, waited.error().message};
        }
        speech_terminal = waited.value().has_value();
      }
      if (!navigation_terminal || !speech_terminal) co_await execution.DelayFor(kPollInterval);
    }
    if (execution.cancelled()) {
      if (speech && !speech_terminal) (void)execution.CancelPrimitive(speech.value());
      if (!navigation_terminal) (void)execution.CancelPrimitive(navigation);
      co_return PrimitiveResult{PrimitiveStatus::kCancelled, "cancelled"};
    }
    if (!cancellation_confirmed)
      co_return PrimitiveResult{PrimitiveStatus::kOutcomeUnknown,
                                "navigation cancellation was not confirmed"};
    auto resumed = execution.StartPrimitive({"base.navigate", "1.0.0"}, base_device_id, arguments);
    if (!resumed) co_return PrimitiveResult{PrimitiveStatus::kFailed, resumed.error().message};
    navigation = std::move(resumed.value());
    Checkpoint(execution, *control, "navigation.resumed", 0.55);
  }
  (void)execution.CancelPrimitive(navigation);
  co_return PrimitiveResult{PrimitiveStatus::kCancelled, "cancelled"};
}

Task<PrimitiveResult> Speak(Execution& execution, std::string device_id,
                             std::string text, std::string language,
                             ControlState* control) {
  auto task = CallPrimitive(execution, "speaker.speak", "1.0.0", device_id,
                            Json{{"text", text}, {"language", language}, {"voice", "default"}},
                            control);
  co_return co_await task;
}

Task<bool> WaitForAsrAfter(Execution& execution, ControlState* control, const std::string& uri,
                           std::uint64_t baseline_sequence, Clock::time_point deadline,
                           Json* recognized, std::string* error) {
  while (Clock::now() < deadline) {
    auto runnable = WaitUntilRunnable(execution, control, error);
    if (!co_await runnable) co_return false;
    Json current;
    if (!ReadJson(execution, uri, &current, error)) co_return false;
    if (current.value("sequence", std::uint64_t{0}) > baseline_sequence &&
        !current.value("text", "").empty()) {
      *recognized = std::move(current);
      co_return true;
    }
    co_await execution.DelayFor(kPollInterval);
  }
  co_return false;
}

SwitchRequest ParseSwitchRequest(std::string_view text) {
  const bool negates_on =
      ContainsAny(text, {"不要开", "不用开", "别开", "不要打开", "不用打开", "别打开", "不要开启"});
  const bool requests_off =
      negates_on || ContainsAny(text, {"关闭", "关掉", "关机", "关上"}) || text == "关";
  const bool requests_on =
      !negates_on && (ContainsAny(text, {"打开", "开启", "开机", "开一下"}) || text == "开");
  if (requests_on == requests_off) return SwitchRequest::kAmbiguous;
  return requests_on ? SwitchRequest::kOn : SwitchRequest::kOff;
}

const char* SwitchRequestName(SwitchRequest request) {
  if (request == SwitchRequest::kOn) return "on";
  if (request == SwitchRequest::kOff) return "off";
  return "ambiguous";
}

}  // namespace island::skills::assist

#pragma once

#include <chrono>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "src/eos/runtime/skills/builtin/registry.h"
#include "share/skills/native/common/primitive_support.h"

namespace island::skills::assist {

using Clock = std::chrono::steady_clock;
using eos::runtime::skills::builtin::Execution;
using eos::runtime::skills::builtin::PrimitiveOperation;
using eos::runtime::skills::builtin::Task;
using Json = nlohmann::json;

inline constexpr double kMaximumApproachDistanceM = 5.0;
inline constexpr double kCenterDeadbandFraction = 0.10;
inline constexpr double kMinimumDetectionConfidence = 0.40;
inline constexpr auto kDetectionTimeout = std::chrono::seconds(60);
inline constexpr auto kAlignmentTimeout = std::chrono::seconds(60);
inline constexpr auto kAsrTimeout = std::chrono::seconds(60);
inline constexpr auto kPollInterval = std::chrono::milliseconds(50);

struct ControlState {
  explicit ControlState(std::string skill_name) : skill(std::move(skill_name)) {}

  std::string skill;
  bool paused{false};
  std::string signal_error;
  std::optional<std::string> source_execution_id;
  std::optional<std::string> correlation_id;
};

struct Snapshot {
  Json value;
  std::uint64_t source_sequence{0};
};

enum class PrimitiveStatus {
  kSucceeded,
  kRejected,
  kFailed,
  kCancelled,
  kTimedOut,
  kOutcomeUnknown,
  kTargetLost
};

struct PrimitiveResult {
  PrimitiveStatus status{PrimitiveStatus::kFailed};
  std::string message;
};

enum class SwitchRequest { kOn, kOff, kAmbiguous };
enum class HumanCondition { kAny, kFallen, kSitting };

Task<bool> WaitUntilRunnable(Execution& execution, ControlState* control,
                             std::string* error = nullptr);
void Checkpoint(Execution& execution, const ControlState& control, std::string_view name,
                double progress, std::string_view state = "progress");

const char* SafeCapabilityError(const eos::runtime::skills::builtin::Error& error);
bool ReadJson(Execution& execution, const std::string& uri, Json* value,
              std::string* error = nullptr);
bool ReadDetection(Execution& execution, const std::string& uri, Snapshot* snapshot,
                   std::string* error = nullptr);
bool ReadJoint4(Execution& execution, const std::string& uri, double* joint4_rad,
                std::string* error = nullptr);
Task<bool> WaitForDetectionAfter(Execution& execution, ControlState* control, const std::string& uri,
                                 std::uint64_t previous_sequence, Clock::time_point deadline,
                                 Snapshot* fresh, std::string* error = nullptr);

const Json* FindHuman(const Json& snapshot, const std::string& expected_id);
bool MatchesAction(const Json& detection, std::string_view action,
                   std::string_view legacy_activity);
bool MatchesPose(const Json& detection, std::string_view pose,
                 std::string_view legacy_activity);
bool MatchesCondition(const Json& detection, HumanCondition condition);
bool ReadNumber(const Json& object, const char* key, double* output);
std::optional<bool> CenteredInImage(
    const Json& snapshot, const Json& human,
    double deadband_fraction = kCenterDeadbandFraction);

Task<PrimitiveResult> CallPrimitive(Execution& execution, const char* primitive,
                                     std::string version, std::string device_id,
                                     Json arguments, ControlState* control);
Task<PrimitiveResult> NavigateWhileObserving(Execution& execution, ControlState* control,
                                              std::string base_device_id,
                                              Json arguments,
                                              std::string detection_uri,
                                              std::string target_id, Snapshot* latest,
                                               HumanCondition condition = HumanCondition::kAny,
                                               std::string expected_name = {},
                                               std::string required_action = {},
                                               std::string* reassociated_target_id = nullptr,
                                                bool cancel_on_invalid_target = true,
                                                std::string head_detection_uri = {},
                                                std::string speaker_device_id = {});
Task<PrimitiveResult> Speak(Execution& execution, std::string device_id,
                             std::string text, std::string language,
                             ControlState* control);

Task<bool> WaitForAsrAfter(Execution& execution, ControlState* control, const std::string& uri,
                           std::uint64_t baseline_sequence, Clock::time_point deadline,
                           Json* recognized, std::string* error = nullptr);
SwitchRequest ParseSwitchRequest(std::string_view text);
const char* SwitchRequestName(SwitchRequest request);

}  // namespace island::skills::assist

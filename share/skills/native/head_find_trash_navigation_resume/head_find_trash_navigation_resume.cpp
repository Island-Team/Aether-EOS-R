#include "share/skills/native/head_find_trash_navigation_resume/head_find_trash_navigation_resume.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "share/skills/native/common/navigation_trash_support.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native::head_find_trash_navigation_resume {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {
using namespace std::chrono_literals;

constexpr auto kPollInterval = 20ms;
constexpr auto kPrimitiveWait = 50ms;
using navigation_trash::kRequiredAbsentFrames;
using navigation_trash::kRequiredConsecutiveFrames;
using navigation_trash::kTrash;

struct NavigationBinding {
  std::string command_id;
  nlohmann::json target;
};

bool ValidTarget(const nlohmann::json& target) {
  if (!target.is_object() || target.value("reference", "") != "absolute" ||
      target.value("frame_id", "") != "map" || target.size() != 5)
    return false;
  for (const char* key : {"x_m", "y_m", "yaw_rad"}) {
    if (!target.contains(key) || !target.at(key).is_number() ||
        !std::isfinite(target.at(key).get<double>()))
      return false;
  }
  return true;
}

bool ValidBinding(const nlohmann::json& value) {
  return value.contains("navigation_command_id") && value.at("navigation_command_id").is_string() &&
         !value.at("navigation_command_id").get_ref<const std::string&>().empty() &&
         value.at("navigation_command_id").get_ref<const std::string&>().size() <= 128 &&
         value.contains("resume_target") && ValidTarget(value.at("resume_target"));
}

void Warning(Execution& execution, const std::string& reason) {
  (void)execution.Checkpoint({"navigation.warning", reason, {}, {{"reason", reason}}});
}

std::string TerminalStatus(const nlohmann::json& acknowledgement) {
  if (!acknowledgement.is_object() || !acknowledgement.contains("result") ||
      !acknowledgement.at("result").is_object())
    return "invalid";
  const auto& result = acknowledgement.at("result");
  const auto status = result.find("status");
  return status != result.end() && status->is_string() ? status->get<std::string>() : "invalid";
}

Task<Outcome> Monitor(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  const std::string base = arguments.at("base_device_id").get<std::string>();
  const std::string uri = "robots/" + robot + "/detection/head/state";

  NavigationBinding binding;
  const bool owns_initial_navigation = arguments.contains("start_relative_target") ||
                                       arguments.contains("start_waypoint_id");
  if (arguments.contains("start_relative_target") && arguments.contains("start_waypoint_id"))
    co_return Outcome::InvalidArgument("navigation.binding_invalid",
                                       "only one owned navigation target may be supplied");
  if (owns_initial_navigation &&
       (arguments.contains("navigation_command_id") || arguments.contains("resume_target")))
    co_return Outcome::InvalidArgument("navigation.binding_invalid",
                                       "start target cannot be combined with an external command");
  if (arguments.contains("navigation_command_id") || arguments.contains("resume_target")) {
    if (!ValidBinding(arguments))
      co_return Outcome::InvalidArgument(
          "navigation.binding_invalid",
          "navigation command ID and absolute map target are required together");
    binding.command_id = arguments.at("navigation_command_id").get<std::string>();
    binding.target = arguments.at("resume_target");
  }
  std::optional<PrimitiveOperation> owned_navigation;
  if (owns_initial_navigation) {
    if (arguments.contains("start_waypoint_id")) {
      const std::string waypoint_id = arguments.at("start_waypoint_id").get<std::string>();
      auto waypoint = execution.ReadResource(
          "robots/" + robot + "/navigation/waypoints/" + waypoint_id);
      if (!waypoint)
        co_return Outcome::Failed("waypoint.unavailable", waypoint.error().message);
      if (!ValidTarget(waypoint.value()))
        co_return Outcome::InvalidArgument("waypoint.invalid",
                                           "waypoint must be an absolute map target");
      binding.target = std::move(waypoint.value());
    } else {
      auto status = execution.ReadResource("robots/" + robot + "/base/status");
      if (!status)
        co_return Outcome::Failed("navigation.localization_unavailable", status.error().message);
       auto target = navigation_trash::AbsoluteMapTarget(status.value(), arguments.at("start_relative_target"));
      if (!target)
        co_return Outcome::Failed("navigation.localization_unavailable",
                                  "fresh localized map pose is required");
      binding.target = std::move(*target);
    }
    auto started = execution.StartPrimitive({"base.navigate", "1.0.0"}, base, binding.target);
    if (!started) co_return Outcome::Failed("navigation.start_failed", started.error().message);
    owned_navigation = std::move(started.value());
  }
  std::optional<ResourceSubscription> subscription;
  if (auto opened = execution.OpenResourceSubscription(uri))
    subscription.emplace(std::move(opened.value()));
  std::uint64_t last_publication_sequence = 0;
  bool read_subscription_baseline = false;
  std::optional<std::uint64_t> last_source_sequence;
  std::array<std::size_t, kTrash.size()> consecutive{};
  std::array<std::size_t, kTrash.size()> absent{};
  std::array<bool, kTrash.size()> announced{};

  (void)execution.Checkpoint({"search.started", {}, 0.1, {}});
  while (true) {
    if (execution.cancelled()) {
      if (owned_navigation) (void)execution.CancelPrimitive(*owned_navigation);
      co_return Outcome::Cancelled();
    }
    while (true) {
      auto received = execution.ReceiveSignal(0ms);
      if (!received)
        co_return Outcome::Failed("navigation.signal_failed", received.error().message);
      if (!received.value()) break;
      if (received.value()->name != "navigation.update") continue;
      if (!ValidBinding(received.value()->payload)) {
        Warning(execution, "navigation.binding_invalid");
        continue;
      }
      binding.command_id = received.value()->payload.at("navigation_command_id").get<std::string>();
      binding.target = received.value()->payload.at("resume_target");
      (void)execution.Checkpoint({"navigation.updated", {}, {}, {}});
    }
    if (owned_navigation) {
      auto waited = execution.WaitPrimitive(*owned_navigation, 0ms);
      if (!waited) {
        Warning(execution, "navigation.resume_wait_failed");
        owned_navigation.reset();
      } else if (waited.value()) {
        const std::string status = TerminalStatus(*waited.value());
        if (owns_initial_navigation && status == "succeeded")
          co_return Outcome::Completed({{"destination_reached", true}, {"target", binding.target}});
        if (owns_initial_navigation && status != "cancelled")
          co_return Outcome::Failed("navigation.failed", "owned navigation did not reach target");
        if (status != "succeeded") Warning(execution, "navigation.resume_failed");
        owned_navigation.reset();
      }
    }

    std::optional<nlohmann::json> snapshot;
    if (!subscription) {
      auto read = execution.ReadResource(uri);
      if (!read) co_return Outcome::Failed("detection.read_failed", read.error().message);
      snapshot = std::move(read.value());
    } else if (!read_subscription_baseline) {
      read_subscription_baseline = true;
      auto history = execution.ReadResourceHistory(*subscription,
                                                   ResourceSelector{ResourceSelectorKind::kLatest});
      if (!history) co_return Outcome::Failed("detection.read_failed", history.error().message);
      if (!history.value().publications.empty()) {
        auto publication = std::move(history.value().publications.front());
        last_publication_sequence = publication.sequence;
        snapshot = std::move(publication.value);
      }
    } else {
      auto waited = execution.WaitResource(*subscription, last_publication_sequence, kPollInterval);
      if (!waited) co_return Outcome::Failed("detection.read_failed", waited.error().message);
      if (waited.value().status == ResourceWaitStatus::kPublication && waited.value().publication) {
        last_publication_sequence = waited.value().publication->sequence;
        snapshot = std::move(waited.value().publication->value);
      }
    }
    if (!snapshot) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }
    const auto sequence = navigation_trash::SourceSequence(*snapshot);
    if (!sequence)
      co_return Outcome::Failed("detection.invalid",
                                "detection snapshot has no valid source_sequence");
    if (last_source_sequence && *sequence <= *last_source_sequence) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }
    last_source_sequence = sequence;
    std::array<bool, kTrash.size()> present;
    if (!navigation_trash::SelectTrash(*snapshot, &present))
      co_return Outcome::Failed("detection.invalid", "detection snapshot has no detections array");
    for (std::size_t index = 0; index < present.size(); ++index) {
      if (announced[index]) {
        if (present[index]) {
          absent[index] = 0;
        } else if (++absent[index] >= kRequiredAbsentFrames) {
          announced[index] = false;
          absent[index] = 0;
          consecutive[index] = 0;
        }
        continue;
      }
      if (!present[index]) {
        consecutive[index] = 0;
        continue;
      }
      if (++consecutive[index] < kRequiredConsecutiveFrames) continue;

      announced[index] = true;
      absent[index] = 0;
      (void)execution.Checkpoint({"trash.found", {}, 0.6, {{"label", kTrash[index].label}}});
      const nlohmann::json resume_target = binding.target;
      auto speech = execution.StartPrimitive({"speaker.play", "1.0.0"}, speaker,
                                             {{"uri", kTrash[index].uri}});
      bool cancelled_navigation = false;
      if (owned_navigation) {
        auto targeted = execution.CancelNavigationCommand(owned_navigation->command_id, base);
        cancelled_navigation = targeted && targeted.value() == NavigationCancelStatus::kCancelled;
        if (!cancelled_navigation) (void)execution.CancelPrimitive(*owned_navigation);
        bool settled = false;
        while (!execution.cancelled()) {
          auto waited = execution.WaitPrimitive(*owned_navigation, kPrimitiveWait);
          if (!waited) break;
          if (waited.value()) {
            settled = true;
            if (targeted && targeted.value() == NavigationCancelStatus::kNotFound &&
                TerminalStatus(*waited.value()) == "cancelled")
              cancelled_navigation = true;
            break;
          }
          co_await execution.DelayFor(kPollInterval);
        }
        if (!settled || !cancelled_navigation) Warning(execution, "navigation.cancel_uncertain");
        cancelled_navigation = settled && cancelled_navigation;
        owned_navigation.reset();
        binding.command_id.clear();
      } else if (!binding.command_id.empty()) {
        const auto cancelled = execution.CancelNavigationCommand(binding.command_id, base);
        if (!cancelled) {
          Warning(execution, "navigation.cancel_uncertain");
        } else {
          switch (cancelled.value()) {
            case NavigationCancelStatus::kCancelled:
              cancelled_navigation = true;
              break;
            case NavigationCancelStatus::kNotFound:
              Warning(execution, "navigation.command_not_found");
              break;
            case NavigationCancelStatus::kMismatch:
              Warning(execution, "navigation.command_mismatch");
              break;
            case NavigationCancelStatus::kUncertain:
              Warning(execution, "navigation.cancel_uncertain");
              break;
          }
        }
        binding.command_id.clear();
      } else {
        Warning(execution, "navigation.command_not_found");
      }

      bool announcement_succeeded = false;
      if (speech) {
        while (!execution.cancelled()) {
          auto waited = execution.WaitPrimitive(speech.value(), kPrimitiveWait);
          if (!waited) {
            Warning(execution, "announcement.failed");
            break;
          }
          if (waited.value()) {
            announcement_succeeded = TerminalStatus(*waited.value()) == "succeeded";
            if (!announcement_succeeded) Warning(execution, "announcement.failed");
            break;
          }
          co_await execution.DelayFor(kPollInterval);
        }
      } else {
        Warning(execution, "announcement.failed");
      }
      if (execution.cancelled()) {
        if (speech) (void)execution.CancelPrimitive(speech.value());
        co_return Outcome::Cancelled();
      }
      if (announcement_succeeded)
        (void)execution.Checkpoint({"announcement.completed", {}, 0.6, {}});
      if (cancelled_navigation && !resume_target.is_null()) {
        auto resumed = execution.StartPrimitive({"base.navigate", "1.0.0"}, base, resume_target);
        if (resumed) {
          owned_navigation = std::move(resumed.value());
          if (execution.cancelled()) {
            (void)execution.CancelPrimitive(*owned_navigation);
            co_return Outcome::Cancelled();
          }
          (void)execution.Checkpoint({"navigation.resumed", {}, {}, {}});
        } else {
          Warning(execution, "navigation.resume_failed");
        }
      }
    }
    co_await execution.DelayFor(kPollInterval);
  }
}

}  // namespace

void RegisterSkill(Registry& registry) {
  Definition skill;
  skill.metadata.name = "head_find_trash_navigation_resume";
  skill.metadata.version = "1.0.0";
  skill.metadata.description =
      "Announces head-camera trash while interrupting and resuming a supplied navigation goal.";
  skill.metadata.library = "island.head_find_trash_navigation_resume";
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  const nlohmann::json target_schema = {
      {"type", "object"},
      {"properties",
       {{"reference", {{"const", "absolute"}}},
        {"frame_id", {{"const", "map"}}},
        {"x_m", {{"type", "number"}}},
        {"y_m", {{"type", "number"}}},
        {"yaw_rad", {{"type", "number"}}}}},
      {"required", {"reference", "frame_id", "x_m", "y_m", "yaw_rad"}},
      {"additionalProperties", false}};
  const nlohmann::json binding_properties = {
      {"navigation_command_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
      {"resume_target", target_schema}};
  skill.metadata.parameters_schema = {
      {"type", "object"},
      {"properties",
       {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
        {"speaker_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"base_device_id", {{"type", "string"}, {"minLength", 1}}},
        {"navigation_command_id", binding_properties.at("navigation_command_id")},
         {"resume_target", target_schema},
         {"start_waypoint_id", {{"type", "string"}, {"minLength", 1}}},
         {"start_relative_target",
         {{"type", "object"},
          {"properties",
           {{"x_m", {{"type", "number"}}},
            {"y_m", {{"type", "number"}}},
            {"yaw_rad", {{"type", "number"}}}}},
          {"required", {"x_m", "y_m", "yaw_rad"}},
          {"additionalProperties", false}}}}},
      {"required", {"robot_name", "speaker_device_id", "base_device_id"}},
      {"dependencies",
       {{"navigation_command_id", {"resume_target"}},
        {"resume_target", {"navigation_command_id"}}}},
      {"additionalProperties", false}};
  skill.metadata.returns_schema = {{"type", "object"}};
  skill.metadata.accepted_signals.push_back(
      {"navigation.update",
       {{"type", "object"},
        {"properties", binding_properties},
        {"required", {"navigation_command_id", "resume_target"}},
        {"additionalProperties", false}}});
  skill.metadata.supports_signals = true;
  skill.metadata.primitives = {{"speaker.play", "1.0.0"}, {"base.navigate", "1.0.0"}};
  skill.metadata.resources.push_back(
      {"robots/{robot_name}/detection/head/state", "Head trash detections."});
  skill.metadata.resources.push_back(
      {"robots/{robot_name}/base/status", "Localized starting pose for owned navigation."});
  skill.metadata.resources.push_back(
      {"robots/{robot_name}/navigation/waypoints/{start_waypoint_id}",
       "Configured absolute map target for owned waypoint navigation."});
  skill.metadata.reentrant = true;
  skill.handler = Monitor;
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native::head_find_trash_navigation_resume

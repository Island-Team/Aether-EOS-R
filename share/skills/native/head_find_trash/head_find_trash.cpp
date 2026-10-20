/// Continuously watch the head-camera detection feed and announce trash encounters.
#include "share/skills/native/head_find_trash/head_find_trash.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "share/skills/native/common/primitive_support.h"
#include "src/eos/runtime/skills/builtin/registry.h"

namespace island::skills::native {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
namespace {
using namespace std::chrono_literals;

constexpr auto kPollInterval = 20ms;
constexpr double kMinimumConfidence = 0.40;
constexpr std::size_t kRequiredConsecutiveFrames = 3;
constexpr std::size_t kRequiredAbsentFrames = 10;

struct TrashDefinition {
  const char* label;
  const char* announcement;
  const char* uri;
};

constexpr std::array<TrashDefinition, 4> kSupportedTrash{{
    {"peel", "主人，发现果皮啦", "asset://head_find_trash/found_peel.wav"},
    {"empty_box", "主人，发现空盒子啦", "asset://head_find_trash/found_empty_box.wav"},
    {"bottle", "发现空瓶了", "asset://head_find_trash/found_bottle.wav"},
    {"paper_ball", "主人，发现纸团啦", "asset://head_find_trash/found_paper_ball.wav"},
}};

struct TrashSelection {
  bool found{false};
  std::string label;
  const char* announcement{nullptr};
  const char* uri{nullptr};
  double confidence{0.0};
  int object_count{0};
};

std::optional<std::size_t> TrashIndex(const std::string& label) {
  for (std::size_t index = 0; index < kSupportedTrash.size(); ++index) {
    if (label == kSupportedTrash[index].label) return index;
  }
  return std::nullopt;
}

bool SelectTrash(const nlohmann::json& snapshot,
                 std::array<TrashSelection, kSupportedTrash.size()>* selections) {
  const auto detections = snapshot.find("detections");
  if (detections == snapshot.end() || !detections->is_array()) return false;
  for (std::size_t index = 0; index < selections->size(); ++index) {
    (*selections)[index] = {};
    (*selections)[index].label = kSupportedTrash[index].label;
    (*selections)[index].announcement = kSupportedTrash[index].announcement;
    (*selections)[index].uri = kSupportedTrash[index].uri;
  }
  int object_count = 0;
  for (const auto& item : *detections) {
    if (!item.is_object() || item.value("kind", "") != "object") continue;
    const auto index = TrashIndex(item.value("label", ""));
    if (!index || !item.contains("confidence") || !item.at("confidence").is_number()) continue;
    const double confidence = item.at("confidence").get<double>();
    if (confidence < kMinimumConfidence || confidence > 1.0) continue;
    ++object_count;
    auto& selection = (*selections)[*index];
    if (!selection.found || confidence > selection.confidence) {
      selection.found = true;
      selection.confidence = confidence;
    }
  }
  for (auto& selection : *selections) selection.object_count = object_count;
  return true;
}

std::optional<std::uint64_t> SourceSequence(const nlohmann::json& snapshot) {
  const auto found = snapshot.find("source_sequence");
  if (found == snapshot.end() || !found->is_number_integer()) return std::nullopt;
  if (found->is_number_unsigned()) return found->get<std::uint64_t>();
  const auto value = found->get<std::int64_t>();
  return value < 0 ? std::nullopt : std::optional<std::uint64_t>(value);
}

nlohmann::json FoundResult(const TrashSelection& trash) {
  return {{"found", true},
          {"label", trash.label},
          {"announcement", trash.announcement},
          {"confidence", trash.confidence},
          {"object_count", trash.object_count}};
}

nlohmann::json NotFoundResult() { return {{"found", false}, {"object_count", 0}}; }

Outcome Rejected(const TrashSelection& trash) {
  return {RunResult::Status::kCompleted, false, "announcement.rejected", FoundResult(trash),
          "speech announcement was rejected"};
}

Task<Outcome> HeadFindTrash(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot = arguments.at("robot_name").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  const bool stop_after_first_detection =
      arguments.value("stop_after_first_detection", false);
  const auto search_timeout =
      std::chrono::milliseconds(arguments.value("search_timeout_ms", 0));
  const auto search_deadline = std::chrono::steady_clock::now() + search_timeout;
  const std::string uri = "robots/" + robot + "/detection/head/state";

  (void)execution.Checkpoint({"search.started", {}, 0.1, {}});
  std::optional<ResourceSubscription> detection_subscription;
  if (auto opened = execution.OpenResourceSubscription(uri)) {
    detection_subscription.emplace(std::move(opened.value()));
  }
  std::uint64_t last_publication_sequence = 0;
  bool read_subscription_baseline = false;
  std::optional<std::uint64_t> last_sequence;
  std::array<std::size_t, kSupportedTrash.size()> consecutive_frames{};
  std::array<std::size_t, kSupportedTrash.size()> absent_frames{};
  std::array<bool, kSupportedTrash.size()> announced{};
  bool stable_trash_found = false;

  while (true) {
    if (execution.cancelled()) co_return Outcome::Cancelled();
    if (!stable_trash_found && search_timeout.count() != 0 &&
        std::chrono::steady_clock::now() >= search_deadline) {
      co_return Outcome{RunResult::Status::kCompleted, false, "trash.search_timeout",
                        NotFoundResult(), "timed out waiting for stable trash detection"};
    }
    std::optional<nlohmann::json> snapshot;
    if (!detection_subscription) {
      auto read = execution.ReadResource(uri);
      if (!read) co_return Outcome::Failed("detection.read_failed", read.error().message);
      snapshot = std::move(read.value());
    } else if (!read_subscription_baseline) {
      read_subscription_baseline = true;
      auto history = execution.ReadResourceHistory(
          *detection_subscription, ResourceSelector{ResourceSelectorKind::kLatest});
      if (!history)
        co_return Outcome::Failed("detection.read_failed", history.error().message);
      if (!history.value().publications.empty()) {
        auto publication = std::move(history.value().publications.front());
        last_publication_sequence = publication.sequence;
        snapshot = std::move(publication.value);
      }
    } else {
      auto waited = execution.WaitResource(*detection_subscription, last_publication_sequence,
                                           kPollInterval);
      if (!waited) co_return Outcome::Failed("detection.read_failed", waited.error().message);
      if (waited.value().status == ResourceWaitStatus::kPublication &&
          waited.value().publication) {
        last_publication_sequence = waited.value().publication->sequence;
        snapshot = std::move(waited.value().publication->value);
      }
    }
    if (!snapshot) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }
    const auto source_sequence = SourceSequence(*snapshot);
    if (!source_sequence)
      co_return Outcome::Failed("detection.invalid",
                                "detection snapshot has no valid source_sequence");
    if (last_sequence && *source_sequence <= *last_sequence) {
      co_await execution.DelayFor(kPollInterval);
      continue;
    }
    last_sequence = source_sequence;

    std::array<TrashSelection, kSupportedTrash.size()> selections;
    if (!SelectTrash(*snapshot, &selections))
      co_return Outcome::Failed("detection.invalid", "detection snapshot has no detections array");
    for (std::size_t index = 0; index < selections.size(); ++index) {
      auto& trash = selections[index];
      if (announced[index]) {
        if (trash.found) {
          absent_frames[index] = 0;
        } else if (++absent_frames[index] >= kRequiredAbsentFrames) {
          announced[index] = false;
          absent_frames[index] = 0;
          consecutive_frames[index] = 0;
        }
        continue;
      }
      if (!trash.found) {
        consecutive_frames[index] = 0;
        continue;
      }
      if (++consecutive_frames[index] < kRequiredConsecutiveFrames) continue;
      if (execution.cancelled()) co_return Outcome::Cancelled();
      if (search_timeout.count() != 0 && std::chrono::steady_clock::now() >= search_deadline) {
        co_return Outcome{RunResult::Status::kCompleted, false, "trash.search_timeout",
                          NotFoundResult(), "timed out waiting for stable trash detection"};
      }
      stable_trash_found = true;

      (void)execution.Checkpoint({"trash.found", {}, 0.6, {}});
      auto task = island::skills::primitive::Run(
          execution, {"speaker.play", "1.0.0"}, speaker, {{"uri", trash.uri}});
      const auto result = co_await task;
      using PrimitiveStatus = island::skills::primitive::Status;
      if (result.status == PrimitiveStatus::kCancelled)
        co_return Outcome::Cancelled("cancelled", result.message);
      if (result.status == PrimitiveStatus::kRejected) co_return Rejected(trash);
      if (result.status != PrimitiveStatus::kSucceeded) {
        const char* suffix = result.status == PrimitiveStatus::kTimedOut         ? "timed_out"
                              : result.status == PrimitiveStatus::kOutcomeUnknown ? "outcome_unknown"
                                                                                  : "failed";
        co_return Outcome::Failed(std::string("announcement.") + suffix,
                                  "announcement playback failed: " + result.message);
      }

      announced[index] = true;
      absent_frames[index] = 0;
      (void)execution.Checkpoint({"announcement.completed", {}, 0.6, {}});
      if (stop_after_first_detection) co_return Outcome::Completed(FoundResult(trash));
    }
    co_await execution.DelayFor(kPollInterval);
  }
}

}  // namespace

void RegisterHeadFindTrash(Registry& registry) {
  Definition definition;
  definition.metadata.name = "head_find_trash";
  definition.metadata.version = "1.0.0";
  definition.metadata.description =
      "Continuously watches head-camera trash detections and announces stable encounters.";
  definition.metadata.library = "island.head_find_trash";
  definition.metadata.library_version = "1.0.0";
  definition.metadata.build_id = "island-source";
  definition.metadata.parameters_schema = {
      {"type", "object"},
      {"properties",
        {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
         {"speaker_device_id", {{"type", "string"}, {"minLength", 1}}},
         {"stop_after_first_detection", {{"type", "boolean"}, {"default", false}}},
         {"search_timeout_ms",
          {{"type", "integer"},
           {"default", 0},
           {"anyOf",
            {{{"const", 0}}, {{"minimum", 1000}, {"maximum", 300000}}}}}}}},
      {"required", {"robot_name", "speaker_device_id"}},
      {"additionalProperties", false}};
  definition.metadata.returns_schema = {
      {"type", "object"},
      {"properties",
       {{"found", {{"type", "boolean"}}},
        {"label",
         {{"type", "string"},
          {"enum", {"peel", "empty_box", "bottle", "paper_ball"}}}},
        {"announcement", {{"type", "string"}}},
        {"confidence", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
        {"object_count", {{"type", "integer"}, {"minimum", 0}}}}},
      {"required", {"found", "object_count"}},
      {"additionalProperties", false}};
  definition.metadata.primitives.push_back({"speaker.play", "1.0.0"});
  definition.metadata.resources.push_back({"robots/{robot_name}/detection/head/state",
                                            "Trash object detections from the head camera."});
  definition.metadata.reentrant = true;
  definition.handler = HeadFindTrash;
  registry.Register(std::move(definition));
}

}  // namespace island::skills::native

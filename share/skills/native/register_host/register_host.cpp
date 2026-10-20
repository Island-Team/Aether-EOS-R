#include "src/eos/runtime/skills/builtin/registry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "share/skills/native/common/primitive_support.h"
#include "share/skills/native/register_host/name_parser.h"
namespace island::skills::native::register_host {
using namespace ::eos::runtime::skills;
using namespace ::eos::runtime::skills::builtin;
using namespace std::chrono_literals;
namespace {

constexpr std::string_view kSkillName = "register_host";
constexpr auto kPoll = 50ms;

struct Control {
  bool paused{false};
};

void EmitEvent(Execution& execution, std::string_view phase, std::string_view state,
               double progress) {
  (void)execution.EmitSignal("skill.event", {{"skill", kSkillName},
                                               {"phase", phase},
                                               {"state", state},
                                               {"progress", progress}});
}

void Checkpoint(Execution& execution, std::string name, double progress, std::size_t stage,
                std::size_t count, std::string_view state = "progress") {
  (void)execution.Checkpoint(
      {name, {}, progress, {{"stage", stage}, {"stage_count", count}}});
  EmitEvent(execution, name, state, progress);
}

void StageFailed(Execution& execution, std::size_t stage, std::size_t count,
                 std::string_view reason, std::string_view message) {
  const double progress = static_cast<double>(stage) / count;
  (void)execution.Checkpoint({"registration.stage.failed", std::string(message), progress,
                              {{"stage", stage}, {"stage_count", count}, {"reason", reason}}});
  EmitEvent(execution, "registration.stage.failed", "progress", progress);
}

Result<void> PollControl(Execution& execution, Control* control, std::string_view phase,
                         double progress, std::chrono::milliseconds timeout = 0ms) {
  while (true) {
    auto received = execution.ReceiveSignal(timeout);
    timeout = 0ms;
    if (!received) return Result<void>::Failure(received.error());
    if (!received.value()) return Result<void>::Success();
    if (received.value()->name != "skill.command") continue;
    const std::string command = received.value()->payload.value("command", "");
    if (command == "pause" && !control->paused) {
      control->paused = true;
      EmitEvent(execution, phase, "paused", progress);
    } else if (command == "resume" && control->paused) {
      control->paused = false;
      EmitEvent(execution, phase, "resumed", progress);
    }
  }
}

Task<Outcome> WaitUntilRunning(Execution& execution, Control* control, std::string_view phase,
                               double progress) {
  while (true) {
    auto polled = PollControl(execution, control, phase, progress, control->paused ? kPoll : 0ms);
    if (!polled) co_return Outcome::Failed("signal.failed", polled.error().message);
    if (execution.cancelled()) co_return Outcome::Cancelled();
    if (!control->paused) co_return Outcome::Completed();
    co_await execution.DelayFor(kPoll);
  }
}

Task<island::skills::primitive::Result> CallPrimitive(Execution& execution, Control* control,
                                  PrimitiveReference primitive, const std::string& device_id,
                                  nlohmann::json arguments, std::string_view phase,
                                  double progress) {
  auto task = island::skills::primitive::Run(
      execution, primitive, device_id, std::move(arguments), [&]() -> Result<bool> {
        auto polled = PollControl(execution, control, phase, progress);
        if (!polled) return Result<bool>::Failure(polled.error());
        return Result<bool>::Success(!control->paused);
      });
  co_return co_await task;
}

Outcome CapabilityFailure(const island::skills::primitive::Result& call, std::string reason,
                           nlohmann::json result,
                           std::string_view stage) {
  using PrimitiveStatus = island::skills::primitive::Status;
  if (call.status == PrimitiveStatus::kCancelled)
    return Outcome::Cancelled("cancelled", call.message);
  if (call.status == PrimitiveStatus::kRejected) {
    Outcome outcome = Outcome::Rejected(std::move(reason),
                                        std::string(stage) + ": " + call.message);
    outcome.result = std::move(result);
    return outcome;
  }
  if (call.status == PrimitiveStatus::kTimedOut) reason += ".timed_out";
  if (call.status == PrimitiveStatus::kOutcomeUnknown) reason += ".outcome_unknown";
  return Outcome::Failed(std::move(reason), std::string(stage) + ": " + call.message);
}

Task<Outcome> RegisterHost(Execution& execution) {
  const auto& arguments = execution.arguments();
  const std::string robot_name = arguments.at("robot_name").get<std::string>();
  const std::string camera = arguments.at("camera_device_id").get<std::string>();
  const std::string speaker = arguments.at("speaker_device_id").get<std::string>();
  const auto introduction_timeout =
      std::chrono::milliseconds(arguments.value("introduction_timeout_ms", 60000));
  const auto host_ready_delay =
      std::chrono::milliseconds(arguments.value("host_ready_delay_ms", 0));
  constexpr std::array<const char*, 3> kArrival{"请主人1就位", "请主人2就位", "请主人3就位"};
  const std::string asr_uri = "robots/" + robot_name + "/world/asr/state";
  auto opened = execution.OpenResourceSubscription(asr_uri);
  if (!opened)
    co_return Outcome::Failed("asr.subscription_failed", opened.error().message);
  ResourceSubscription asr = std::move(opened.value());
  Control control;
  nlohmann::json registered = nlohmann::json::array();
  Checkpoint(execution, "registration.started", 0.0, 0, kArrival.size(), "started");

  for (std::size_t index = 0; index < kArrival.size(); ++index) {
    const double progress = static_cast<double>(index) / kArrival.size();
    auto running = WaitUntilRunning(execution, &control, "registration.stage", progress);
    auto ready = co_await running;
    if (ready.status != RunResult::Status::kCompleted) co_return ready;
    Checkpoint(execution, "registration.stage.started", progress, index + 1, kArrival.size());

    auto arrival_task = CallPrimitive(execution, &control, {"speaker.speak", "1.0.0"}, speaker,
                                      {{"text", kArrival[index]}, {"language", "zh-CN"}},
                                      "arrival.prompt", progress);
    auto arrival = co_await arrival_task;
    if (arrival.status != island::skills::primitive::Status::kSucceeded) {
      if (arrival.status == island::skills::primitive::Status::kCancelled)
        co_return Outcome::Cancelled("cancelled", arrival.message);
      StageFailed(execution, index + 1, kArrival.size(), "prompt.failed", arrival.message);
      continue;
    }
    if (host_ready_delay > 0ms) {
      co_await execution.DelayFor(host_ready_delay);
      if (execution.cancelled()) co_return Outcome::Cancelled();
    }

    // Capture the boundary before speaking: a host may answer while the prompt is playing.
    auto baseline = execution.ReadResourceHistory(
        asr, ResourceSelector{ResourceSelectorKind::kLatest});
    if (!baseline) co_return Outcome::Failed("asr.read_failed", baseline.error().message);
    std::uint64_t asr_sequence = baseline.value().retention.latest_sequence;

    auto introduction_task = CallPrimitive(
        execution, &control, {"speaker.speak", "1.0.0"}, speaker,
        {{"text", "请自我介绍"}, {"language", "zh-CN"}}, "introduction.prompt", progress);
    auto introduction = co_await introduction_task;
    if (introduction.status != island::skills::primitive::Status::kSucceeded) {
      if (introduction.status == island::skills::primitive::Status::kCancelled)
        co_return Outcome::Cancelled("cancelled", introduction.message);
      StageFailed(execution, index + 1, kArrival.size(), "introduction_prompt.failed",
                  introduction.message);
      continue;
    }

    std::optional<std::string> introduced_name;
    auto introduction_deadline = std::chrono::steady_clock::now() + introduction_timeout;
    while (!introduced_name) {
      const auto pause_started = std::chrono::steady_clock::now();
      auto wait_task = WaitUntilRunning(execution, &control, "introduction.wait", progress);
      auto waiting = co_await wait_task;
      if (waiting.status != RunResult::Status::kCompleted) co_return waiting;
      const auto now = std::chrono::steady_clock::now();
      introduction_deadline += now - pause_started;
      if (now >= introduction_deadline) {
        StageFailed(execution, index + 1, kArrival.size(), "asr.timeout",
                    "Timed out waiting for introduction");
        break;
      }

      auto next = execution.WaitResource(
          asr, asr_sequence,
          std::min(kPoll, std::chrono::duration_cast<std::chrono::milliseconds>(
                              introduction_deadline - now)));
      if (!next) co_return Outcome::Failed("asr.wait_failed", next.error().message);
      if (next.value().status == ResourceWaitStatus::kCancelled || execution.cancelled())
        co_return Outcome::Cancelled();
      if (next.value().status == ResourceWaitStatus::kClosed)
        co_return Outcome::Failed("asr.closed", "ASR resource subscription closed");
      if (next.value().status != ResourceWaitStatus::kPublication ||
          !next.value().publication) {
        continue;
      }
      asr_sequence = next.value().publication->sequence;
      const auto& value = next.value().publication->value;
      if (value.is_object()) introduced_name = ParseIntroducedName(value.value("text", ""));
    }
    if (!introduced_name) continue;

    auto prompt_task = CallPrimitive(execution, &control, {"speaker.speak", "1.0.0"}, speaker,
                                     {{"text", "开始注册"}, {"language", "zh-CN"}},
                                     "registration.prompt", progress);
    auto prompt = co_await prompt_task;
    if (prompt.status != island::skills::primitive::Status::kSucceeded) {
      if (prompt.status == island::skills::primitive::Status::kCancelled)
        co_return Outcome::Cancelled("cancelled", prompt.message);
      StageFailed(execution, index + 1, kArrival.size(), "registration_prompt.failed",
                  prompt.message);
      continue;
    }

    auto face_task = CallPrimitive(execution, &control, {"face.register", "1.0.0"}, camera,
                                   {{"name", *introduced_name}}, "face.registration", progress);
    auto face = co_await face_task;
    if (face.status != island::skills::primitive::Status::kSucceeded) {
      if (face.status == island::skills::primitive::Status::kCancelled)
        co_return Outcome::Cancelled("cancelled", face.message);
      StageFailed(execution, index + 1, kArrival.size(), "face_registration.failed",
                  face.message);
      continue;
    }
    registered.push_back(*introduced_name);

    auto completion_task = CallPrimitive(
        execution, &control, {"speaker.speak", "1.0.0"}, speaker,
        {{"text", "注册完成"}, {"language", "zh-CN"}}, "completion.prompt", progress);
    auto completion = co_await completion_task;
    if (completion.status == island::skills::primitive::Status::kCancelled)
      co_return Outcome::Cancelled("cancelled", completion.message);
    Checkpoint(execution, "registration.stage.completed",
                static_cast<double>(index + 1) / kArrival.size(), index + 1, kArrival.size());
  }

  const std::string final_text = registered.size() == kArrival.size()
                                     ? "三个主人全部注册完成"
                                     : "主人注册流程已完成";
  auto final_task = CallPrimitive(
      execution, &control, {"speaker.speak", "1.0.0"}, speaker,
      {{"text", final_text}, {"language", "zh-CN"}}, "final.prompt", 1.0);
  auto final = co_await final_task;
  if (final.status != island::skills::primitive::Status::kSucceeded)
    co_return CapabilityFailure(final, "final_prompt.failed", {{"registered_hosts", registered}},
                                "final prompt");
  Checkpoint(execution, "registration.completed", 1.0, kArrival.size(), kArrival.size(),
             "completed");
  co_return Outcome::Completed({{"registered_hosts", registered}});
}

}  // namespace

void RegisterSkill(Registry& registry) {
  Definition skill;
  skill.metadata.name = "register_host";
  skill.metadata.version = "1.0.0";
  skill.metadata.description =
      "Collects spoken host names and registers three faces in sequence.";
  skill.metadata.library = "island.register_host";
  skill.metadata.library_version = "1.0.0";
  skill.metadata.build_id = "island-source";
  skill.metadata.parameters_schema = {
      {"type", "object"},
       {"properties", {{"robot_name", {{"type", "string"}, {"minLength", 1}}},
                        {"camera_device_id", {{"type", "string"}, {"minLength", 1}}},
                        {"speaker_device_id", {{"type", "string"}, {"minLength", 1}}},
                        {"host_ready_delay_ms", {{"type", "integer"},
                                                 {"minimum", 0},
                                                 {"maximum", 30000},
                                                 {"default", 0}}},
                        {"introduction_timeout_ms", {{"type", "integer"},
                                                    {"minimum", 1000},
                                                    {"maximum", 300000},
                                                    {"default", 60000}}}}},
      {"required", {"robot_name", "camera_device_id", "speaker_device_id"}},
      {"additionalProperties", false}};
  skill.metadata.returns_schema = {
      {"type", "object"},
      {"properties", {{"registered_hosts", {{"type", "array"},
                                               {"items", {{"type", "string"}}}}},
                      {"failed_host", {{"type", "string"}}}}},
      {"required", {"registered_hosts"}},
      {"additionalProperties", false}};
  skill.metadata.primitives = {{"face.register", "1.0.0"}, {"speaker.speak", "1.0.0"}};
  skill.metadata.claim_templates = {
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/camera_device_id"},
      {ClaimKind::kDevice, ClaimMode::kExclusiveUse, "/speaker_device_id"}};
  skill.metadata.resources = {
      {"robots/{robot_name}/world/asr/state", "Latest recognized speech result."}};
  skill.metadata.accepted_signals.push_back(
      {"skill.command", {{"type", "object"},
                         {"properties", {{"command", {{"enum", {"pause", "resume"}}}}}},
                         {"required", {"command"}},
                         {"additionalProperties", false}}});
  skill.metadata.emitted_signals.push_back(
      {"skill.event", {{"type", "object"},
                       {"properties", {{"skill", {{"const", "register_host"}}},
                                       {"phase", {{"type", "string"}, {"minLength", 1}}},
                                       {"state", {{"enum", {"started", "progress", "paused",
                                                               "resumed", "completed"}}}},
                                       {"progress", {{"type", "number"}, {"minimum", 0},
                                                     {"maximum", 1}}}}},
                       {"required", {"skill", "phase", "state", "progress"}},
                       {"additionalProperties", false}}});
  skill.metadata.supports_signals = true;
  skill.metadata.reentrant = true;
  skill.handler = RegisterHost;
  registry.Register(std::move(skill));
}

}  // namespace island::skills::native::register_host

#include "src/eos/runtime/skills/builtin/registry.h"
#include "src/eos/lib/timer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <coroutine>
#include <future>
#include <thread>
#include <type_traits>

namespace eos::runtime::skills::builtin {
namespace {

class BuiltinExecutionCorrectnessTest : public testing::Test {
 protected:
  eos::lib::ManualTimerService timer_;
};

template <typename T>
concept HasPublishResource = requires(T value) {
  value.PublishResource("resource", nlohmann::json::object());
};

template <typename T>
concept HasPublishResourceCapability = requires(T value) { value.publish_resource; };

static_assert(std::is_move_constructible_v<ResourceSubscription>);
static_assert(std::is_move_assignable_v<ResourceSubscription>);
static_assert(!std::is_copy_constructible_v<ResourceSubscription>);
static_assert(!std::is_copy_assignable_v<ResourceSubscription>);
static_assert(!HasPublishResource<Execution>);
static_assert(!HasPublishResourceCapability<RuntimeCapabilities>);

Definition TestDefinition(bool reentrant, Handler handler) {
  Definition definition;
  definition.metadata.name = "test.correctness";
  definition.metadata.version = "1.0.0";
  definition.metadata.description = "Exercises built-in execution ownership";
  definition.metadata.parameters_schema = {{"type", "object"}};
  definition.metadata.returns_schema = {{"type", "object"}};
  definition.metadata.reentrant = reentrant;
  definition.handler = std::move(handler);
  return definition;
}

struct CaptureContinuation {
  std::coroutine_handle<>* continuation;
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<> value) const { *continuation = value; }
  void await_resume() const noexcept {}
};

TEST_F(BuiltinExecutionCorrectnessTest, DetectionOverlayClearsAtExecutionEnd) {
  std::string active_id;
  int clear_count = 0;
  RuntimeCapabilities capabilities;
  capabilities.set_detection_overlay = [&](const std::string& id, nlohmann::json correction) {
    active_id = id;
    EXPECT_EQ(correction["source"], "camera-1");
  };
  capabilities.clear_detection_overlay = [&](const std::string& id) {
    EXPECT_EQ(id, "execution-1");
    active_id.clear();
    ++clear_count;
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto set = execution.SetDetectionOverlay({{"source", "camera-1"}});
    if (!set) co_return Outcome::Failed("overlay_set_failed");
    co_return Outcome::Completed();
  });
  definition.metadata.name = "observe_human";
  registry.Register(std::move(definition));
  ExecutionHooks hooks;
  hooks.execution_id = "execution-1";
  auto result = registry.ExecuteWithHooks("observe_human", "1.0.0", nlohmann::json::object(),
                                          "command-1", std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(active_id.empty());
  EXPECT_EQ(clear_count, 1);
}

TEST_F(BuiltinExecutionCorrectnessTest, DelayedExecutionsSuspendAndCompleteExactlyOnce) {
  constexpr int kExecutionCount = 8;
  Registry registry(timer_);
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    co_await execution.DelayFor(std::chrono::milliseconds(20));
    co_return Outcome::Completed(nlohmann::json::object());
  }));
  std::vector<RunResult> results;
  for (int index = 0; index < kExecutionCount; ++index) {
    registry.ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(),
                          "delay-" + std::to_string(index), {},
                          [&](RunResult result) { results.push_back(std::move(result)); });
  }

  EXPECT_TRUE(results.empty());
  timer_.AdvanceBy(std::chrono::milliseconds(19));
  EXPECT_TRUE(results.empty());
  timer_.AdvanceBy(std::chrono::milliseconds(1));
  ASSERT_EQ(results.size(), kExecutionCount);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
    EXPECT_TRUE(result.accepted);
  }
  timer_.AdvanceBy(std::chrono::hours(1));
  EXPECT_EQ(results.size(), kExecutionCount);
}

TEST(BuiltinExecutionATimerTest, RegistryDestructionWakesDelaysAfterTimerShutdown) {
  eos::lib::ManualTimerService timer;
  auto registry = std::make_unique<Registry>(timer);
  registry->Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    co_await execution.DelayFor(std::chrono::hours(1));
    co_return Outcome::Completed(nlohmann::json::object());
  }));
  std::atomic<int> completions{0};
  for (int index = 0; index < 3; ++index) {
    registry->ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(),
                           "shutdown-" + std::to_string(index), {},
                           [&](RunResult) { ++completions; });
  }

  EXPECT_EQ(completions.load(), 0);
  timer.Shutdown();
  EXPECT_EQ(completions.load(), 0);
  registry.reset();
  EXPECT_EQ(completions.load(), 3);
}

TEST(BuiltinExecutionATimerTest, DestructionRacingTimerFireCompletesExactlyOnce) {
  for (int run = 0; run < 20; ++run) {
    eos::lib::ManualTimerService timer;
    auto registry = std::make_unique<Registry>(timer);
    registry->Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
      co_await execution.DelayFor(std::chrono::milliseconds(1));
      co_return Outcome::Completed(nlohmann::json::object());
    }));
    std::atomic<int> completions{0};
    registry->ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(),
                           "race-" + std::to_string(run), {},
                           [&](RunResult) { ++completions; });
    ASSERT_EQ(completions.load(), 0);

    std::barrier start(2);
    std::thread destroyer([&] {
      start.arrive_and_wait();
      registry.reset();
    });
    start.arrive_and_wait();
    timer.AdvanceBy(std::chrono::milliseconds(1));
    destroyer.join();

    EXPECT_EQ(completions.load(), 1) << "run " << run;
    timer.AdvanceBy(std::chrono::hours(1));
    EXPECT_EQ(completions.load(), 1) << "run " << run;
  }
}

TEST(BuiltinExecutionATimerTest, TimerCapacityRejectionCompletesWithFailureExactlyOnce) {
  eos::lib::TimerOptions options;
  options.max_timers = 1;
  eos::lib::ManualTimerService timer(options);
  auto occupied = timer.ScheduleAfter(std::chrono::hours(1), [] {});
  ASSERT_TRUE(occupied);
  Registry registry(timer);
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    co_await execution.DelayFor(std::chrono::milliseconds(1));
    co_return Outcome::Completed(nlohmann::json::object());
  }));
  std::vector<RunResult> results;

  registry.ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(),
                        "capacity-rejected", {},
                        [&](RunResult result) { results.push_back(std::move(result)); });

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results.front().status, RunResult::Status::kFailed);
  EXPECT_EQ(results.front().reason_code, "handler_exception");
  timer.AdvanceBy(std::chrono::hours(1));
  EXPECT_EQ(results.size(), 1u);
}

TEST_F(BuiltinExecutionCorrectnessTest, NonReentrantAsyncRunsAreSerializedUntilCompletion) {
  Registry registry(timer_);
  std::coroutine_handle<> first_continuation;
  std::atomic<int> entered{0};
  registry.Register(TestDefinition(false, [&](Execution&) -> Task<Outcome> {
    if (++entered == 1) co_await CaptureContinuation{&first_continuation};
    co_return Outcome::Completed(nlohmann::json::object());
  }));
  std::atomic<int> completed{0};
  registry.ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(), "first", {},
                        [&](RunResult) { ++completed; });
  registry.ExecuteAsync("test.correctness", "1.0.0", nlohmann::json::object(), "second", {},
                        [&](RunResult) { ++completed; });

  ASSERT_TRUE(first_continuation);
  EXPECT_EQ(entered.load(), 1);
  EXPECT_EQ(completed.load(), 0);
  first_continuation.resume();
  EXPECT_EQ(entered.load(), 2);
  EXPECT_EQ(completed.load(), 2);
}

TEST_F(BuiltinExecutionCorrectnessTest, SendReportsHookRejection) {
  Registry registry(timer_);
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto sent = execution.Send({"child"}, "input", nlohmann::json::object(), "request",
                               std::chrono::seconds(1));
    if (!sent) co_return Outcome::Rejected(sent.error().code, sent.error().message);
    co_return Outcome::Completed(nlohmann::json::object());
  }));
  ExecutionHooks hooks;
  hooks.send_message = [](const ProcessHandle&, const std::string&, nlohmann::json,
                          const std::string&, std::chrono::milliseconds) { return false; };
  const auto result = registry.ExecuteWithHooks("test.correctness", "1.0.0",
                                                nlohmann::json::object(), "send", hooks);
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.reason_code, "send_rejected");
}

TEST_F(BuiltinExecutionCorrectnessTest, RelatedStateReadsUseHandlesAndRevisionWaits) {
  Registry registry(timer_);
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    if (!execution.parent_process()) co_return Outcome::Failed("test.parent_handle_missing");
    const auto read = execution.ReadRelatedProcessState(*execution.parent_process());
    if (!read || !read.value() || read.value()->revision != 1)
      co_return Outcome::Failed("test.parent_read_failed");
    const auto waited = execution.WaitRelatedProcessState(
        *execution.parent_process(), 1, std::chrono::seconds(1));
    if (!waited || !waited.value() || waited.value()->revision != 2)
      co_return Outcome::Failed("test.revision_wait_failed");
    ProcessHandle forged = *execution.parent_process();
    ++forged.generation;
    const auto unauthorized = execution.ReadRelatedProcessState(forged);
    if (unauthorized || unauthorized.error().code != "state_read_failed")
      co_return Outcome::Failed("test.forged_handle_accepted");
    co_return Outcome::Completed();
  }));
  ExecutionHooks hooks;
  hooks.parent_process = ProcessHandle{"parent", {}, 7};
  hooks.read_related_process_state = [](const ProcessHandle& process)
      -> std::optional<RetainedExecutionState> {
    if (process.generation != 7) throw std::invalid_argument("invalid process handle");
    return RetainedExecutionState{1, {}, {{"value", 1}}};
  };
  hooks.wait_related_process_state = [](const ProcessHandle& process, std::uint64_t after,
                                        std::chrono::milliseconds)
      -> std::optional<RetainedExecutionState> {
    if (process.generation != 7) throw std::invalid_argument("invalid process handle");
    if (after >= 2) return std::nullopt;
    return RetainedExecutionState{2, {}, {{"value", 2}}};
  };
  const auto result = registry.ExecuteWithHooks("test.correctness", "1.0.0",
                                                nlohmann::json::object(), "related-state", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
}

TEST_F(BuiltinExecutionCorrectnessTest, ConcurrentRequestsMatchRepliesByCorrelationId) {
  Registry registry(timer_);
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    const ProcessHandle child{"child", "parent", 2};
    auto first = std::async(std::launch::async, [&] {
      return execution.Request(child, "same", {{"value", 1}}, "request-1",
                               std::chrono::seconds(1));
    });
    auto second = std::async(std::launch::async, [&] {
      return execution.Request(child, "same", {{"value", 2}}, "request-2",
                               std::chrono::seconds(1));
    });
    const auto first_reply = first.get();
    const auto second_reply = second.get();
    if (!first_reply || !second_reply || first_reply.value().correlation_id != "request-1" ||
        second_reply.value().correlation_id != "request-2")
      co_return Outcome::Failed("test.reply_mismatch");
    co_return Outcome::Completed();
  }));
  ExecutionHooks hooks;
  hooks.send_message = [](const ProcessHandle&, const std::string&, nlohmann::json,
                          const std::string&, std::chrono::milliseconds) { return true; };
  hooks.receive_reply = [](const ProcessHandle& process, const std::string& channel,
                            const std::string& correlation_id,
                            std::chrono::milliseconds) -> std::optional<IpcMessage> {
    EXPECT_EQ(process.execution_id, "child");
    EXPECT_EQ(process.generation, 2u);
    if (correlation_id == "request-1") std::this_thread::sleep_for(std::chrono::milliseconds(10));
    IpcMessage reply;
    reply.sender_execution_id = process.execution_id;
    reply.channel = channel;
    reply.correlation_id = correlation_id;
    return reply;
  };
  const auto result = registry.ExecuteWithHooks("test.correctness", "1.0.0",
                                                nlohmann::json::object(), "requests", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_TRUE(result.accepted);
}

TEST_F(BuiltinExecutionCorrectnessTest, PublishesSchemaValidatedStateThroughScopedHooks) {
  Registry registry(timer_);
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto published = execution.PublishState({{"phase", "working"}});
    if (!published) co_return Outcome::Failed(published.error().code, published.error().message);
    auto related = execution.PublishRelatedProcessState({{"token", 7}});
    if (!related) co_return Outcome::Failed(related.error().code, related.error().message);
    auto invalid = execution.PublishState({{"phase", 4}});
    if (invalid) co_return Outcome::Failed("test.invalid_state_accepted");
    co_return Outcome::Completed({{"error", invalid.error().code}});
  });
  definition.metadata.execution_state_schema = {
      {"type", "object"},
      {"properties", {{"phase", {{"type", "string"}}}}},
      {"required", {"phase"}},
      {"additionalProperties", false}};
  definition.metadata.related_process_state_schema = {
      {"type", "object"},
      {"properties", {{"token", {{"type", "integer"}}}}},
      {"required", {"token"}},
      {"additionalProperties", false}};
  registry.Register(std::move(definition));

  nlohmann::json public_state;
  nlohmann::json related_state;
  ExecutionHooks hooks;
  hooks.publish_execution_state = [&](nlohmann::json value) {
    public_state = std::move(value);
    return true;
  };
  hooks.publish_related_process_state = [&](nlohmann::json value) {
    related_state = std::move(value);
    return true;
  };
  const auto result = registry.ExecuteWithHooks("test.correctness", "1.0.0",
                                                nlohmann::json::object(), "state", hooks);
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("error"), "state_publish_failed");
  EXPECT_EQ(public_state, nlohmann::json({{"phase", "working"}}));
  EXPECT_EQ(related_state, nlohmann::json({{"token", 7}}));
}

TEST_F(BuiltinExecutionCorrectnessTest, RejectsMalformedOptionalStateSchemaAtRegistration) {
  Registry registry(timer_);
  auto definition = TestDefinition(true, [](Execution&) -> Task<Outcome> {
    co_return Outcome::Completed();
  });
  definition.metadata.execution_state_schema = {{"type", "invalid"}};
  EXPECT_THROW(registry.Register(std::move(definition)), std::invalid_argument);
}

TEST_F(BuiltinExecutionCorrectnessTest, ResourceSubscriptionHoldsDemandAcrossHistoryAndWaits) {
  struct Demand {
    explicit Demand(std::atomic<int>* released) : released(released) {}
    ~Demand() { ++*released; }
    std::atomic<int>* released;
  };

  std::atomic<int> released{0};
  std::weak_ptr<Demand> demand;
  std::vector<ResourceSelectorKind> selectors;
  std::atomic<int> waits{0};
  RuntimeCapabilities capabilities;
  capabilities.open_resource_subscription = [&](const std::string& uri) {
    EXPECT_EQ(uri, "robots/demo/detection/state");
    auto held = std::make_shared<Demand>(&released);
    demand = held;
    RuntimeCapabilities::ResourceAccess access;
    access.lifetime = held;
    access.read_history = [&](const ResourceSelector& selector) {
      EXPECT_FALSE(demand.expired());
      selectors.push_back(selector.kind);
      return ResourceHistory{{ResourcePublication{7, {}, "boot", "epoch", {{"value", 7}}}},
                             {3, 9, 2, false}, false, false};
    };
    access.wait_publication = [&](std::uint64_t after, std::chrono::milliseconds timeout,
                                  Cancelled cancelled) {
      EXPECT_FALSE(demand.expired());
      EXPECT_EQ(after, 7u);
      EXPECT_EQ(timeout, std::chrono::milliseconds(20));
      EXPECT_FALSE(cancelled && cancelled());
      ++waits;
      return ResourceWait{ResourceWaitStatus::kPublication,
                          ResourcePublication{8, {}, "boot", "epoch", {{"value", 8}}},
                          {3, 9, 2, false}, false};
    };
    return access;
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto opened = execution.OpenResourceSubscription("robots/demo/detection/state");
    if (!opened) co_return Outcome::Failed(opened.error().code);
    ResourceSubscription subscription = std::move(opened.value());
    if (opened.value() || !subscription) co_return Outcome::Failed("test.invalid_move_state");

    const auto second = std::chrono::system_clock::time_point(std::chrono::seconds(2));
    const auto third = std::chrono::system_clock::time_point(std::chrono::seconds(3));
    const ResourceSelector queries[] = {
        {ResourceSelectorKind::kLatest},
        {ResourceSelectorKind::kExactSequence, 7},
        {ResourceSelectorKind::kExactTime, 0, 0, second},
        {ResourceSelectorKind::kBefore, 0, 0, second},
        {ResourceSelectorKind::kAfter, 0, 0, second},
        {ResourceSelectorKind::kNearest, 0, 0, second},
        {ResourceSelectorKind::kSequenceRange, 3, 9, {}, {}, 4},
        {ResourceSelectorKind::kTimeRange, 0, 0, second, third, 4},
    };
    for (const auto& query : queries) {
      auto history = execution.ReadResourceHistory(subscription, query);
      if (!history || history.value().publications.size() != 1 ||
          history.value().publications.front().sequence != 7)
        co_return Outcome::Failed("test.history_read_failed");
    }
    auto waited = execution.WaitResource(subscription, 7, std::chrono::milliseconds(20));
    if (!waited || waited.value().status != ResourceWaitStatus::kPublication ||
        !waited.value().publication || waited.value().publication->sequence != 8)
      co_return Outcome::Failed("test.resource_wait_failed");
    auto closed = execution.CloseResourceSubscription(subscription);
    if (!closed || subscription) co_return Outcome::Failed("test.resource_close_failed");
    auto after_close = execution.ReadResourceHistory(subscription, {});
    if (after_close || after_close.error().code != "subscription_closed")
      co_return Outcome::Failed("test.closed_subscription_accepted");
    co_return Outcome::Completed();
  });
  definition.metadata.resources.push_back(
      {"robots/{robot_name}/detection/state", "Detection history"});
  registry.Register(std::move(definition));

  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "resource-subscription");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(selectors, (std::vector<ResourceSelectorKind>{
                           ResourceSelectorKind::kLatest,
                           ResourceSelectorKind::kExactSequence,
                           ResourceSelectorKind::kExactTime,
                           ResourceSelectorKind::kBefore,
                           ResourceSelectorKind::kAfter,
                           ResourceSelectorKind::kNearest,
                           ResourceSelectorKind::kSequenceRange,
                           ResourceSelectorKind::kTimeRange}));
  EXPECT_EQ(waits, 1);
  EXPECT_TRUE(demand.expired());
  EXPECT_EQ(released, 1);
}

TEST_F(BuiltinExecutionCorrectnessTest, ResourceSubscriptionEnforcesDeclarationAndBounds) {
  std::atomic<int> opens{0};
  std::atomic<int> reads{0};
  std::atomic<int> waits{0};
  RuntimeCapabilities capabilities;
  capabilities.open_resource_subscription = [&](const std::string&) {
    ++opens;
    RuntimeCapabilities::ResourceAccess access;
    access.lifetime = std::make_shared<int>(1);
    access.read_history = [&](const ResourceSelector&) {
      ++reads;
      return ResourceHistory{};
    };
    access.wait_publication = [&](std::uint64_t, std::chrono::milliseconds, Cancelled) {
      ++waits;
      return ResourceWait{};
    };
    return access;
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto oversized = execution.OpenResourceSubscription(std::string(4097, 'x'));
    if (oversized || oversized.error().code != "invalid_resource_uri")
      co_return Outcome::Failed("test.oversized_resource_uri_accepted");
    auto undeclared = execution.OpenResourceSubscription("robots/demo/private/state");
    if (undeclared || undeclared.error().code != "capability_undeclared")
      co_return Outcome::Failed("test.undeclared_resource_accepted");
    auto opened = execution.OpenResourceSubscription("robots/demo/detection/state");
    if (!opened) co_return Outcome::Failed(opened.error().code);
    auto& subscription = opened.value();
    ResourceSelector exact{ResourceSelectorKind::kExactSequence};
    ResourceSelector sequence_range{ResourceSelectorKind::kSequenceRange, 1, 300, {}, {}, 257};
    ResourceSelector reversed_time{ResourceSelectorKind::kTimeRange};
    reversed_time.first_time = std::chrono::system_clock::time_point(std::chrono::seconds(2));
    reversed_time.last_time = std::chrono::system_clock::time_point(std::chrono::seconds(1));
    for (const auto& invalid : {exact, sequence_range, reversed_time}) {
      auto result = execution.ReadResourceHistory(subscription, invalid);
      if (result || result.error().code != "resource_query_failed")
        co_return Outcome::Failed("test.invalid_selector_accepted");
    }
    for (const auto timeout : {std::chrono::milliseconds(-1), std::chrono::milliseconds(60001)}) {
      auto result = execution.WaitResource(subscription, 0, timeout);
      if (result || result.error().code != "invalid_resource_wait")
        co_return Outcome::Failed("test.invalid_wait_accepted");
    }
    co_return Outcome::Completed();
  });
  definition.metadata.resources.push_back(
      {"robots/{robot_name}/detection/state", "Detection history"});
  registry.Register(std::move(definition));

  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "resource-bounds");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
  EXPECT_EQ(opens, 1);
  EXPECT_EQ(reads, 0);
  EXPECT_EQ(waits, 0);
}

TEST_F(BuiltinExecutionCorrectnessTest, RejectsInvalidResourceMetadataAtRegistration) {
  Registry registry(timer_);
  auto malformed = TestDefinition(true, [](Execution&) -> Task<Outcome> {
    co_return Outcome::Completed();
  });
  malformed.metadata.resources.push_back({"robots/{missing/state", "Malformed"});
  EXPECT_THROW(registry.Register(std::move(malformed)), std::invalid_argument);

  auto duplicate = TestDefinition(true, [](Execution&) -> Task<Outcome> {
    co_return Outcome::Completed();
  });
  duplicate.metadata.resources.push_back({"resource", "First"});
  duplicate.metadata.resources.push_back({"resource", "Second"});
  EXPECT_THROW(registry.Register(std::move(duplicate)), std::invalid_argument);
}

TEST_F(BuiltinExecutionCorrectnessTest, ResourceWaitReceivesExecutionCancellation) {
  RuntimeCapabilities capabilities;
  capabilities.open_resource_subscription = [](const std::string&) {
    RuntimeCapabilities::ResourceAccess access;
    access.lifetime = std::make_shared<int>(1);
    access.read_history = [](const ResourceSelector&) { return ResourceHistory{}; };
    access.wait_publication = [](std::uint64_t, std::chrono::milliseconds, Cancelled cancelled) {
      return ResourceWait{cancelled && cancelled() ? ResourceWaitStatus::kCancelled
                                                   : ResourceWaitStatus::kTimeout};
    };
    return access;
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto opened = execution.OpenResourceSubscription("resource");
    if (!opened) co_return Outcome::Failed(opened.error().code);
    auto waited = execution.WaitResource(opened.value(), 0, std::chrono::seconds(1));
    if (!waited || waited.value().status != ResourceWaitStatus::kCancelled)
      co_return Outcome::Failed("test.cancellation_not_forwarded");
    co_return Outcome::Completed();
  });
  definition.metadata.resources.push_back({"resource", "Test history"});
  registry.Register(std::move(definition));
  ExecutionHooks hooks;
  hooks.cancelled = [] { return true; };

  const auto result = registry.ExecuteWithHooks("test.correctness", "1.0.0",
                                                 nlohmann::json::object(), "resource-cancel",
                                                 std::move(hooks));
  EXPECT_EQ(result.status, RunResult::Status::kCompleted) << result.reason_code;
}

TEST_F(BuiltinExecutionCorrectnessTest, RejectsUndeclaredInferenceBeforeProvider) {
  bool called = false;
  RuntimeCapabilities capabilities;
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    called = true;
    return InferenceResult{request.authority, request.source, request.source_sequence,
                           request.source_generation, nlohmann::json::object()};
  };
  Registry registry(timer_, std::move(capabilities));
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto started = execution.StartInference(
        {InferenceAuthority::kHumanPose, "camera.front", 1, 1, nlohmann::json::object()});
    if (started) co_return Outcome::Failed("test.unexpected");
    co_return Outcome::Completed({{"error", started.error().code}});
  }));
  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "undeclared-inference");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("error"), "capability_undeclared");
  EXPECT_FALSE(called);
}

TEST_F(BuiltinExecutionCorrectnessTest, VadUsesCallerInputWithEmptyProvenance) {
  std::vector<InferenceRequest> requests;
  RuntimeCapabilities capabilities;
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    requests.push_back(request);
    return InferenceResult{request.authority, {}, 0, 0, {{"speech", true}}};
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto invalid = execution.StartInference(
        {InferenceAuthority::kVad, "microphone.front", 1, 1, {{"input", {{"data_b64", "AA=="}}}}});
    if (invalid || invalid.error().code != "invalid_inference_request")
      co_return Outcome::Failed("test.invalid_vad_accepted");
    auto started = execution.StartInference(
        {InferenceAuthority::kVad, {}, 0, 0, {{"input", {{"data_b64", "AA=="}}}}});
    if (!started) co_return Outcome::Failed(started.error().code, started.error().message);
    auto result = execution.WaitInference(started.value(), std::chrono::seconds(1));
    if (!result || !result.value()) co_return Outcome::Failed("test.vad_wait_failed");
    co_return Outcome::Completed(result.value()->value);
  });
  definition.metadata.inference_authorities = {InferenceAuthority::kVad};
  registry.Register(std::move(definition));

  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "vad-inference");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("speech"), true);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(requests.front().source.empty());
  EXPECT_EQ(requests.front().source_sequence, 0u);
  EXPECT_EQ(requests.front().source_generation, 0u);
}

TEST_F(BuiltinExecutionCorrectnessTest, VisionInlineModeRejectsRetainedProvenance) {
  std::size_t calls = 0;
  RuntimeCapabilities capabilities;
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    ++calls;
    return InferenceResult{request.authority, {}, 0, 0, {{"ok", true}}};
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto mixed = execution.StartInference(
        {InferenceAuthority::kHuman, "camera.front", 1, 1,
         {{"input", {{"data_b64", "AAAA"}}}}});
    if (mixed || mixed.error().code != "invalid_inference_request") {
      co_return Outcome::Failed("test.mixed_vision_accepted");
    }
    auto started = execution.StartInference(
        {InferenceAuthority::kHuman, {}, 0, 0, {{"input", {{"data_b64", "AAAA"}}}}});
    if (!started) co_return Outcome::Failed(started.error().code, started.error().message);
    auto result = execution.WaitInference(started.value(), std::chrono::seconds(1));
    if (!result || !result.value()) co_return Outcome::Failed("test.inline_wait_failed");
    co_return Outcome::Completed(result.value()->value);
  });
  definition.metadata.inference_authorities = {InferenceAuthority::kHuman};
  registry.Register(std::move(definition));

  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "inline-vision");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("ok"), true);
  EXPECT_EQ(calls, 1u);
}

TEST_F(BuiltinExecutionCorrectnessTest, ChatUsesTextOptionsWithEmptyProvenance) {
  std::vector<InferenceRequest> requests;
  RuntimeCapabilities capabilities;
  capabilities.infer_once = [&](const InferenceRequest& request, Cancelled) {
    requests.push_back(request);
    return InferenceResult{request.authority, {}, 0, 0, {{"text", "你好"}}};
  };
  Registry registry(timer_, std::move(capabilities));
  auto definition = TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto invalid = execution.StartInference(
        {InferenceAuthority::kChatCompletion, "camera.front", 1, 1,
         {{"messages", nlohmann::json::array()}}});
    if (invalid || invalid.error().code != "invalid_inference_request")
      co_return Outcome::Failed("test.invalid_chat_accepted");
    auto started = execution.StartInference(
        {InferenceAuthority::kChatCompletion, {}, 0, 0,
         {{"messages", {{{"role", "user"}, {"content", "你好"}}}}, {"stream", false}}});
    if (!started) co_return Outcome::Failed(started.error().code, started.error().message);
    auto result = execution.WaitInference(started.value(), std::chrono::seconds(1));
    if (!result || !result.value()) co_return Outcome::Failed("test.chat_wait_failed");
    co_return Outcome::Completed(result.value()->value);
  });
  definition.metadata.inference_authorities = {InferenceAuthority::kChatCompletion};
  registry.Register(std::move(definition));

  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "chat-inference");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("text"), "你好");
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(requests.front().source.empty());
  EXPECT_EQ(requests.front().options.at("stream"), false);
}

TEST_F(BuiltinExecutionCorrectnessTest, ObservesDefaultCameraWithCancellationContext) {
  RuntimeCapabilities capabilities;
  capabilities.default_object_camera = [] { return std::string("cam-0"); };
  capabilities.observe_objects = [](const std::string& device_id, Cancelled cancelled) {
    EXPECT_EQ(device_id, "cam-0");
    EXPECT_FALSE(cancelled && cancelled());
    return nlohmann::json{{"source", device_id}, {"detections", nlohmann::json::array()}};
  };
  Registry registry(timer_, std::move(capabilities));
  registry.Register(TestDefinition(true, [](Execution& execution) -> Task<Outcome> {
    auto camera = execution.DefaultObjectCamera();
    if (!camera) co_return Outcome::Failed(camera.error().code);
    auto observed = execution.ObserveObjects(camera.value());
    if (!observed) co_return Outcome::Failed(observed.error().code);
    co_return Outcome::Completed(observed.value());
  }));
  const auto result = registry.Execute("test.correctness", "1.0.0", nlohmann::json::object(),
                                       "observe-default-camera");
  EXPECT_EQ(result.status, RunResult::Status::kCompleted);
  EXPECT_EQ(result.result.at("source"), "cam-0");
}

}  // namespace
}  // namespace eos::runtime::skills::builtin

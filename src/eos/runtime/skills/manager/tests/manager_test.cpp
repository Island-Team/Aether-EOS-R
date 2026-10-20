#include "src/eos/runtime/skills/manager/manager.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace eos::runtime::skills {
namespace {
class TestProvider final : public Provider {
 public:
  TestProvider() {
    metadata_.name = "test.control";
    metadata_.version = "1.0.0";
    metadata_.library = "test";
    metadata_.library_version = "1";
    metadata_.parameters_schema = {{"type", "object"},
                                   {"properties", {{"resource", {{"type", "string"}}}}},
                                   {"required", {"resource"}}};
    metadata_.returns_schema = {{"type", "object"}};
    metadata_.supports_signals = true;
    metadata_.reentrant = true;
    metadata_.accepted_signals.push_back(
        {"control.update", {{"type", "object"}, {"additionalProperties", true}}});
    metadata_.emitted_signals.push_back(
        {"control.applied", {{"type", "object"}, {"additionalProperties", true}}});
    metadata_.claim_templates.push_back(
        {ClaimKind::kResource, ClaimMode::kExclusiveUse, "/resource"});
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json& arguments) const override {
    if (!arguments.contains("resource")) throw std::invalid_argument("resource required");
  }
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override {
    return {};
  }
  RunResult ExecuteWithHooks(const std::string&, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    ++running;
    auto signal = hooks.receive_signal(std::chrono::seconds(2));
    if (signal) hooks.emit_signal({"control.applied", signal->payload});
    --running;
    RunResult result;
    result.status = RunResult::Status::kCompleted;
    result.accepted = true;
    result.reason_code = "completed";
    return result;
  }
  mutable std::atomic<int> running{0};

 private:
  Metadata metadata_;
};

class RoutedProvider final : public Provider {
 public:
  explicit RoutedProvider(std::string name, bool reentrant = false) {
    metadata_.name = std::move(name);
    metadata_.version = "1.0.0";
    metadata_.parameters_schema = {{"type", "object"}};
    metadata_.returns_schema = {{"type", "object"}};
    metadata_.supports_signals = true;
    metadata_.reentrant = reentrant;
    metadata_.accepted_signals.push_back(
        {"control.update", {{"type", "object"}, {"additionalProperties", true}}});
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string& name, const std::string&,
                         const nlohmann::json&) const override {
    if (name != metadata_.name) throw std::invalid_argument("wrong runtime owner");
    ++validations;
  }
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override {
    return {};
  }
  RunResult ExecuteWithHooks(const std::string& name, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    if (name != metadata_.name) throw std::runtime_error("execution routed to wrong runtime");
    ++executions;
    ++running;
    RunResult result;
    while (true) {
      const auto signal = hooks.receive_signal(std::chrono::milliseconds(10));
      if (signal && signal->name == "CANCEL") {
        ++cancellations;
        result.status = RunResult::Status::kCancelled;
        break;
      }
      if (signal && signal->name == "control.update") {
        ++signals;
        result.status = RunResult::Status::kCompleted;
        result.accepted = true;
        break;
      }
      if (hooks.cancelled()) {
        ++cancellations;
        result.status = RunResult::Status::kCancelled;
        break;
      }
    }
    --running;
    return result;
  }

  mutable std::atomic<int> validations{0};
  mutable std::atomic<int> executions{0};
  mutable std::atomic<int> running{0};
  mutable std::atomic<int> signals{0};
  mutable std::atomic<int> cancellations{0};

 private:
  Metadata metadata_;
};

class SuspendedAsyncProvider final : public Provider {
 public:
  SuspendedAsyncProvider() {
    metadata_.name = "test.suspended";
    metadata_.version = "1.0.0";
    metadata_.parameters_schema = {{"type", "object"}};
    metadata_.returns_schema = {{"type", "object"}};
    metadata_.reentrant = true;
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override {
    throw std::logic_error("async provider used through synchronous boundary");
  }
  void ExecuteAsync(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, ExecutionHooks, RunCompletion completion) const override {
    std::lock_guard lock(mutex_);
    completions_.push_back(std::move(completion));
  }
  std::size_t suspended() const {
    std::lock_guard lock(mutex_);
    return completions_.size();
  }
  void CompleteAll() {
    std::vector<RunCompletion> completions;
    {
      std::lock_guard lock(mutex_);
      completions.swap(completions_);
    }
    for (auto& completion : completions) {
      RunResult result;
      result.status = RunResult::Status::kCompleted;
      result.accepted = true;
      completion(std::move(result));
    }
  }

 private:
  Metadata metadata_;
  mutable std::mutex mutex_;
  mutable std::vector<RunCompletion> completions_;
};

class JournalProvider final : public Provider {
 public:
  JournalProvider() {
    metadata_.name = "test.record";
    metadata_.version = "1.0.0";
    metadata_.parameters_schema = {{"type", "object"}};
    metadata_.returns_schema = {{"type", "object"}};
    metadata_.reentrant = true;
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override {
    return {};
  }
  RunResult ExecuteWithHooks(const std::string&, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    hooks.diagnostic_log(true);
    hooks.diagnostic_log(false);
    EXPECT_FALSE(hooks.receive_signal(std::chrono::milliseconds::zero()));
    hooks.record_capability("resource.read", std::chrono::milliseconds(5));
    hooks.record_capability("primitive.call", std::chrono::milliseconds(7));
    EXPECT_TRUE(hooks.emit_checkpoint({"work.started", "Starting", 0.25, {{"phase", "work"}}}));
    const std::string activity = hooks.begin_activity({"resource.read", {{"template", "x/{id}"}}});
    hooks.end_activity(activity, "succeeded");
    RunResult result;
    result.status = RunResult::Status::kCompleted;
    result.accepted = true;
    result.reason_code = "completed";
    result.message = "record completed";
    return result;
  }

 private:
  Metadata metadata_;
};

class ProcessProvider final : public Provider {
 public:
  ProcessProvider() {
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"value", {{"type", "string"}}}}},
                                   {"required", {"value"}},
                                   {"additionalProperties", false}};
    for (const std::string name : {"test.parent", "test.child"}) {
      Metadata metadata;
      metadata.name = name;
      metadata.version = "1.0.0";
      metadata.parameters_schema = {{"type", "object"}};
      metadata.returns_schema = {{"type", "object"}};
      metadata.reentrant = true;
      metadata.ipc_channels.push_back({"process.echo", schema, true, true});
      metadata_.push_back(std::move(metadata));
    }
  }
  std::vector<Metadata> Discover() const override { return metadata_; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    for (const auto& metadata : metadata_)
      if (metadata.name == name && metadata.version == version) return &metadata;
    return nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override { return {}; }
  RunResult ExecuteWithHooks(const std::string& name, const std::string&,
                             const nlohmann::json&, const std::string&,
                             ExecutionHooks hooks) const override {
    RunResult result;
    result.status = RunResult::Status::kCompleted;
    result.accepted = true;
    if (name == "test.parent") {
      const ProcessHandle child = hooks.spawn("test.child", "1.0.0", nlohmann::json::object(),
                                               "child-request", std::chrono::seconds(2));
      EXPECT_TRUE(hooks.send_message(child, "process.echo", {{"value", "ping"}}, "message-1",
                                     std::chrono::seconds(1)));
      const auto reply = hooks.receive_reply(child, "process.echo", "message-1",
                                             std::chrono::seconds(1));
      EXPECT_TRUE(reply.has_value());
      if (reply) result.result = reply->payload;
      EXPECT_TRUE(hooks.await_exit(child, std::chrono::seconds(1)).has_value());
    } else {
      const auto request = hooks.receive_message("process.echo", std::chrono::seconds(1));
      EXPECT_TRUE(request.has_value());
      if (request) {
        EXPECT_TRUE(hooks.parent_process.has_value());
        if (hooks.parent_process) {
          EXPECT_TRUE(hooks.send_message(*hooks.parent_process, "process.echo", request->payload,
                                         request->correlation_id,
                                         std::chrono::seconds(1)));
        }
      }
    }
    return result;
  }

 private:
  std::vector<Metadata> metadata_;
};

class PassiveProcessProvider final : public Provider {
 public:
  PassiveProcessProvider() {
    const nlohmann::json schema = {{"type", "object"}};
    for (const std::string name : {"passive.parent", "passive.child"}) {
      Metadata metadata;
      metadata.name = name;
      metadata.version = "1.0.0";
      metadata.parameters_schema = schema;
      metadata.returns_schema = schema;
      metadata.reentrant = true;
      metadata.ipc_channels.push_back({"process.test", schema, true, true});
      metadata_.push_back(std::move(metadata));
    }
  }
  std::vector<Metadata> Discover() const override { return metadata_; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    for (const auto& metadata : metadata_)
      if (metadata.name == name && metadata.version == version) return &metadata;
    return nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override { return {}; }
  RunResult ExecuteWithHooks(const std::string&, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    ++executions;
    {
      std::lock_guard lock(mutex_);
      hooks_[hooks.execution_id] = hooks;
    }
    while (!hooks.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    RunResult result;
    result.status = RunResult::Status::kCancelled;
    return result;
  }
  std::optional<ExecutionHooks> Hooks(const std::string& execution_id) const {
    std::lock_guard lock(mutex_);
    const auto found = hooks_.find(execution_id);
    return found == hooks_.end() ? std::nullopt : std::optional<ExecutionHooks>(found->second);
  }
  mutable std::atomic<int> executions{0};

 private:
  std::vector<Metadata> metadata_;
  mutable std::mutex mutex_;
  mutable std::map<std::string, ExecutionHooks> hooks_;
};

class SourceProvider final : public Provider {
 public:
  SourceProvider() {
    metadata_.name = "test.sources";
    metadata_.version = "1.0.0";
    metadata_.parameters_schema = {{"type", "object"}};
    metadata_.returns_schema = {{"type", "object"}};
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"value", {{"type", "integer"}}}}},
                                   {"required", {"value"}}};
    metadata_.sources.push_back({"vision.people", schema, SourceAuthority::kRead});
    metadata_.sources.push_back({"vision.people", schema, SourceAuthority::kWrite});
    metadata_.reentrant = true;
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&,
                         const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override { return {}; }
 private:
  Metadata metadata_;
};

class StateProvider final : public Provider {
 public:
  StateProvider() {
    metadata_.name = "test.state";
    metadata_.version = "1.0.0";
    metadata_.parameters_schema = {{"type", "object"}};
    metadata_.returns_schema = {{"type", "object"}};
    metadata_.execution_state_schema = {
        {"type", "object"},
        {"properties", {{"phase", {{"type", "string"}}}}},
        {"required", {"phase"}},
        {"additionalProperties", false}};
    metadata_.related_process_state_schema = {
        {"type", "object"},
        {"properties", {{"token", {{"type", "integer"}}}}},
        {"required", {"token"}},
        {"additionalProperties", false}};
    metadata_.reentrant = true;
  }
  std::vector<Metadata> Discover() const override { return {metadata_}; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    return name == metadata_.name && version == metadata_.version ? &metadata_ : nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&, const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override { return {}; }
  RunResult ExecuteWithHooks(const std::string&, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    invalid_public_rejected = !hooks.publish_execution_state({{"phase", 3}});
    first_public_accepted = hooks.publish_execution_state({{"phase", "starting"}});
    second_public_accepted = hooks.publish_execution_state({{"phase", "ready"}});
    private_accepted = hooks.publish_related_process_state({{"token", 9}});
    invalid_private_rejected = !hooks.publish_related_process_state({{"token", "secret"}});
    oversized_rejected = !hooks.publish_execution_state({{"phase", std::string(512, 'x')}});
    RunResult result;
    result.status = RunResult::Status::kCompleted;
    result.accepted = true;
    return result;
  }

  mutable bool invalid_public_rejected{false};
  mutable bool first_public_accepted{false};
  mutable bool second_public_accepted{false};
  mutable bool private_accepted{false};
  mutable bool invalid_private_rejected{false};
  mutable bool oversized_rejected{false};

 private:
  Metadata metadata_;
};

class RelatedStateProvider final : public Provider {
 public:
  RelatedStateProvider() {
    for (const std::string name : {"related.parent", "related.child"}) {
      Metadata metadata;
      metadata.name = name;
      metadata.version = "1.0.0";
      metadata.parameters_schema = {{"type", "object"}};
      metadata.returns_schema = {{"type", "object"}};
      metadata.related_process_state_schema = {
          {"type", "object"},
          {"properties", {{"value", {{"type", "integer"}}}}},
          {"required", {"value"}},
          {"additionalProperties", false}};
      metadata.reentrant = true;
      metadata_.push_back(std::move(metadata));
    }
  }
  std::vector<Metadata> Discover() const override { return metadata_; }
  const Metadata* Find(const std::string& name, const std::string& version) const override {
    for (const auto& metadata : metadata_)
      if (metadata.name == name && metadata.version == version) return &metadata;
    return nullptr;
  }
  void ValidateArguments(const std::string&, const std::string&, const nlohmann::json&) const override {}
  RunResult Execute(const std::string&, const std::string&, const nlohmann::json&,
                    const std::string&, Cancelled) const override { return {}; }
  RunResult ExecuteWithHooks(const std::string& name, const std::string&, const nlohmann::json&,
                             const std::string&, ExecutionHooks hooks) const override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hooks_[name] = hooks;
    }
    while (!hooks.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    RunResult result;
    result.status = RunResult::Status::kCancelled;
    result.accepted = true;
    return result;
  }
  std::optional<ExecutionHooks> Hooks(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = hooks_.find(name);
    return found == hooks_.end() ? std::nullopt : std::optional<ExecutionHooks>(found->second);
  }

 private:
  std::vector<Metadata> metadata_;
  mutable std::mutex mutex_;
  mutable std::map<std::string, ExecutionHooks> hooks_;
};

bool WaitUntil(const std::function<bool()>& ready,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return ready();
}

TEST(SkillManagerTest, DeliversAndReplaysTypedSignalsIdempotently) {
  TestProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  options.max_execution_records = 8;
  options.max_pending_signals = 2;
  options.max_journal_entries = 8;
  SkillManager manager(provider, options);
  const auto started = manager.Start("test.control", "1.0.0", {{"resource", "arm"}}, "start");
  ASSERT_TRUE(WaitUntil([&] {
    const auto execution = manager.Get(started.execution.execution_id);
    return execution && execution->status == ExecutionStatus::kRunning;
  }));
  const auto receipt = manager.Signal(started.execution.execution_id, "control.update",
                                      {{"speed", 2}}, "signal-request");
  EXPECT_FALSE(receipt.signal_id.empty());
  const auto replay = manager.Signal(started.execution.execution_id, "control.update",
                                     {{"speed", 2}}, "signal-request");
  EXPECT_EQ(replay.status, SignalReceipt::Status::kExisting);
  EXPECT_EQ(replay.signal_id, receipt.signal_id);
  std::vector<JournalEntry> events;
  std::uint64_t cursor = 1;
  for (int attempt = 0; attempt < 10; ++attempt) {
    auto next = manager.WaitForJournal(started.execution.execution_id, cursor,
                                               std::chrono::milliseconds(200));
    if (!next.empty()) cursor = next.back().sequence;
    events.insert(events.end(), next.begin(), next.end());
    if (std::any_of(events.begin(), events.end(), [](const JournalEntry& event) {
          return event.kind == JournalEntry::Kind::kSignal;
        }))
      break;
  }
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const JournalEntry& event) {
    return event.kind == JournalEntry::Kind::kSignal && event.signal.name == "control.applied" &&
           !event.signal.signal_id.empty();
  }));
}

TEST(SkillManagerSourceTest, RetainsBoundedTimestampedTimelinesAndLooksUpSamples) {
  SourceProvider provider;
  SkillManagerOptions options;
  options.max_source_samples = 2;
  SkillManager manager(provider, options);
  const auto first = manager.PublishSource("vision.people", 100, {{"value", 1}});
  const auto second = manager.PublishSource("vision.people", 300, {{"value", 3}});
  const auto third = manager.PublishSource("vision.people", 200, {{"value", 2}});
  EXPECT_LT(first.sequence, second.sequence);
  EXPECT_LT(second.sequence, third.sequence);
  EXPECT_FALSE(manager.QuerySource("vision.people", SourceLookup::kExact, 100));
  EXPECT_EQ(manager.QuerySource("vision.people", SourceLookup::kLatest)->sequence, third.sequence);
  EXPECT_EQ(manager.QuerySource("vision.people", SourceLookup::kBefore, 250)->value.at("value"), 2);
  EXPECT_EQ(manager.QuerySource("vision.people", SourceLookup::kAfter, 250)->value.at("value"), 3);
  EXPECT_EQ(manager.QuerySource("vision.people", SourceLookup::kNearest, 260)->value.at("value"), 3);
  EXPECT_THROW(manager.PublishSource("vision.people", 400, {{"value", "bad"}}), std::exception);
}

TEST(SkillManagerStateTest, RetainsLatestValidatedBoundedStateThroughTerminalLifetime) {
  StateProvider provider;
  eos::lib::ManualClock clock(eos::lib::MonotonicTime(std::chrono::seconds(5)),
                              eos::lib::WallTime(std::chrono::seconds(10)));
  SkillManagerOptions options;
  options.clock = &clock;
  options.max_execution_state_bytes = 128;
  SkillManager manager(provider, options);
  const auto started =
      manager.Start("test.state", "1.0.0", nlohmann::json::object(), "state-request");
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(started.execution.execution_id);
    return snapshot && IsTerminal(snapshot->status);
  }));

  const auto state = manager.GetExecutionState(started.execution.execution_id);
  ASSERT_TRUE(state);
  EXPECT_EQ(state->revision, 2u);
  EXPECT_EQ(state->published_at, eos::lib::WallTime(std::chrono::seconds(10)));
  EXPECT_EQ(state->payload, nlohmann::json({{"phase", "ready"}}));
  EXPECT_TRUE(provider.invalid_public_rejected);
  EXPECT_TRUE(provider.first_public_accepted);
  EXPECT_TRUE(provider.second_public_accepted);
  EXPECT_TRUE(provider.private_accepted);
  EXPECT_TRUE(provider.invalid_private_rejected);
  EXPECT_TRUE(provider.oversized_rejected);

  const auto waited = manager.WaitForExecutionState(started.execution.execution_id, 0,
                                                     std::chrono::milliseconds(0));
  ASSERT_TRUE(waited);
  EXPECT_EQ(waited->revision, 2u);
  EXPECT_FALSE(manager.WaitForExecutionState(started.execution.execution_id, 2,
                                             std::chrono::milliseconds(0)));
}

TEST(SkillManagerStateTest, RelatedStateRequiresValidDirectProcessHandlesAndWaitsForRevision) {
  RelatedStateProvider provider;
  SkillManagerOptions options;
  options.worker_count = 3;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("related.parent", "1.0.0", nlohmann::json::object(), "related-parent");
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks("related.parent").has_value(); }));
  const auto parent_hooks = *provider.Hooks("related.parent");
  const ProcessHandle child_handle =
      parent_hooks.spawn("related.child", "1.0.0", nlohmann::json::object(),
                         "related-child", std::chrono::seconds(5));
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks("related.child").has_value(); }));
  const auto child_hooks = *provider.Hooks("related.child");
  ASSERT_TRUE(child_hooks.parent_process);

  EXPECT_TRUE(parent_hooks.publish_related_process_state({{"value", 10}}));
  const auto parent_state = child_hooks.read_related_process_state(*child_hooks.parent_process);
  ASSERT_TRUE(parent_state);
  EXPECT_EQ(parent_state->payload.at("value"), 10);

  EXPECT_TRUE(child_hooks.publish_related_process_state({{"value", 20}}));
  const auto child_state = parent_hooks.read_related_process_state(child_handle);
  ASSERT_TRUE(child_state);
  EXPECT_EQ(child_state->revision, 1u);
  EXPECT_FALSE(manager.GetExecutionState(child_handle.execution_id));
  EXPECT_FALSE(manager.ReceiveMessage(parent.execution.execution_id, {},
                                      std::chrono::milliseconds(0)));

  ProcessHandle stale = child_handle;
  ++stale.generation;
  EXPECT_THROW(parent_hooks.read_related_process_state(stale), std::invalid_argument);
  ProcessHandle forged = child_handle;
  forged.parent_execution_id.clear();
  EXPECT_THROW(parent_hooks.read_related_process_state(forged), std::invalid_argument);
  const auto outsider =
      manager.Start("related.parent", "1.0.0", nlohmann::json::object(), "related-outsider");
  ASSERT_TRUE(WaitUntil([&] {
    const auto hooks = provider.Hooks("related.parent");
    return hooks && hooks->execution_id == outsider.execution.execution_id;
  }));
  const auto outsider_hooks = *provider.Hooks("related.parent");
  EXPECT_THROW(outsider_hooks.read_related_process_state(child_handle), std::invalid_argument);

  std::optional<RetainedExecutionState> waited;
  std::thread waiter([&] {
    waited = parent_hooks.wait_related_process_state(child_handle, 1, std::chrono::seconds(1));
  });
  EXPECT_TRUE(child_hooks.publish_related_process_state({{"value", 21}}));
  waiter.join();
  ASSERT_TRUE(waited);
  EXPECT_EQ(waited->revision, 2u);
  EXPECT_EQ(waited->payload.at("value"), 21);

  manager.Terminate(parent.execution.execution_id);
  manager.Terminate(outsider.execution.execution_id);
}

TEST(SkillManagerSourceTest, WaitsForNewerAndObservesCancellation) {
  SourceProvider provider;
  SkillManager manager(provider);
  std::optional<SourceSample> received;
  std::thread waiter([&] {
    received = manager.WaitForSource("vision.people", 0, std::chrono::seconds(1));
  });
  const auto published = manager.PublishSource("vision.people", 42, {{"value", 7}});
  waiter.join();
  ASSERT_TRUE(received);
  EXPECT_EQ(received->sequence, published.sequence);
  EXPECT_FALSE(manager.WaitForSource("vision.people", published.sequence,
                                    std::chrono::seconds(1), [] { return true; }));
}

TEST(SkillManagerSourceTest, AdmitsRuntimeOwnedTopicsAndRejectsNegativeTimestamps) {
  TestProvider provider;
  SkillManagerOptions options;
  options.sources.push_back({"robots.arm.state", {{"type", "object"}}, SourceAuthority::kRead});
  SkillManager manager(provider, options);

  EXPECT_THROW(manager.PublishSource("robots.arm.state", -1, nlohmann::json::object()),
               std::invalid_argument);
  const SourceSample sample =
      manager.PublishSource("robots.arm.state", 42, nlohmann::json::object());
  EXPECT_EQ(sample.source_timestamp_ns, 42);
  EXPECT_EQ(sample.event_time.source_timestamp_ns, 42);
  EXPECT_EQ(sample.event_time.source_clock_id, "device:robots.arm.state");
  EXPECT_EQ(sample.event_time.sequence, sample.sequence);
  EXPECT_FALSE(sample.event_time.boot_id.empty());
  EXPECT_FALSE(sample.event_time.event_id.empty());
}

TEST(SkillManagerJournalTest, ReportsRetentionGapAndAppendTimestamps) {
  TestProvider provider;
  eos::lib::ManualClock clock(eos::lib::MonotonicTime(std::chrono::seconds(5)),
                              eos::lib::WallTime(std::chrono::seconds(10)));
  SkillManagerOptions options;
  options.clock = &clock;
  options.max_journal_entries = 2;
  SkillManager manager(provider, options);
  const auto started = manager.Start("test.control", "1.0.0", {{"resource", "arm"}}, "gap");
  ASSERT_EQ(started.execution.event_time.observed_at,
            eos::lib::WallTime(std::chrono::seconds(10)));
  ASSERT_FALSE(started.execution.boot_id.empty());
  clock.Advance(std::chrono::seconds(1));
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(started.execution.execution_id);
    return snapshot && snapshot->status == ExecutionStatus::kRunning;
  }));
  ASSERT_TRUE(manager.Terminate(started.execution.execution_id));
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(started.execution.execution_id);
    return snapshot && IsTerminal(snapshot->status);
  }));
  const auto events = manager.WaitForJournal(started.execution.execution_id, 1,
                                              std::chrono::milliseconds(0));
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.front().kind, JournalEntry::Kind::kGap);
  EXPECT_EQ(events.front().gap_after_sequence, 1U);
  EXPECT_GT(events.front().gap_first_available_sequence, 1U);
  const auto lifecycle = std::find_if(events.begin(), events.end(), [](const auto& event) {
    return event.kind == JournalEntry::Kind::kLifecycle;
  });
  ASSERT_NE(lifecycle, events.end());
  EXPECT_GE(lifecycle->timestamp, eos::lib::WallTime(std::chrono::seconds(11)));
  EXPECT_EQ(lifecycle->execution.event_time.boot_id, lifecycle->execution.boot_id);
}

TEST(SkillManagerTest, HoldsConflictingClaimsAndCancelsQueuedWork) {
  TestProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  options.max_execution_records = 8;
  options.max_pending_signals = 2;
  options.max_journal_entries = 8;
  SkillManager manager(provider, options);
  const auto first = manager.Start("test.control", "1.0.0", {{"resource", "arm"}}, "first");
  manager.WaitForSnapshots(first.execution.execution_id, 1, std::chrono::seconds(1));
  const auto second = manager.Start("test.control", "1.0.0", {{"resource", "arm"}}, "second");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const auto queued = manager.Get(second.execution.execution_id);
  ASSERT_TRUE(queued);
  EXPECT_EQ(queued->status, ExecutionStatus::kQueued);
  EXPECT_EQ(queued->queue_reason, "claim.conflict");
  EXPECT_TRUE(manager.Terminate(second.execution.execution_id));
  EXPECT_EQ(manager.Get(second.execution.execution_id)->status, ExecutionStatus::kCancelled);
  manager.Terminate(first.execution.execution_id);
}

TEST(SkillManagerTest, ExpiresClaimBlockedWorkWithoutAnotherSchedulerEvent) {
  TestProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(provider, options);
  const auto first = manager.Start("test.control", "1.0.0", {{"resource", "arm"}},
                                   "deadline-holder");
  ASSERT_TRUE(WaitUntil([&] { return provider.running.load() == 1; }));
  const auto second = manager.Start("test.control", "1.0.0", {{"resource", "arm"}},
                                    "deadline-waiter",
                                    StartOptions{std::chrono::milliseconds(40)});

  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(second.execution.execution_id);
    return snapshot && snapshot->status == ExecutionStatus::kTimedOut;
  }));
  const auto expired = manager.Get(second.execution.execution_id);
  ASSERT_TRUE(expired);
  EXPECT_EQ(expired->reason_code, "deadline.exceeded");
  EXPECT_FALSE(expired->started_at.has_value());
  EXPECT_TRUE(expired->finished_at.has_value());
  manager.Terminate(first.execution.execution_id);
}

TEST(SkillManagerTest, OmittedTimeoutHasNoDeadline) {
  SuspendedAsyncProvider provider;
  eos::lib::ManualClock clock(eos::lib::MonotonicTime(std::chrono::seconds(5)),
                              eos::lib::WallTime(std::chrono::seconds(10)));
  SkillManagerOptions options;
  options.clock = &clock;
  SkillManager manager(provider, options);
  const auto started =
      manager.Start("test.suspended", "1.0.0", nlohmann::json::object(), "no-deadline");

  EXPECT_FALSE(started.execution.deadline_at.has_value());
  ASSERT_TRUE(WaitUntil([&] { return provider.suspended() == 1; }));
  clock.Advance(std::chrono::hours(2));
  const auto active = manager.Get(started.execution.execution_id);
  ASSERT_TRUE(active);
  EXPECT_EQ(active->run_duration, std::chrono::hours(2));
  EXPECT_EQ(active->total_duration, std::chrono::hours(2));
  const auto listed = manager.ListExecutions();
  ASSERT_EQ(listed.size(), 1u);
  EXPECT_EQ(listed.front().run_duration, std::chrono::hours(2));
  provider.CompleteAll();

  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(started.execution.execution_id);
    return snapshot && IsTerminal(snapshot->status);
  }));
  EXPECT_EQ(manager.Get(started.execution.execution_id)->status, ExecutionStatus::kSucceeded);
  EXPECT_FALSE(manager.Get(started.execution.execution_id)->deadline_at.has_value());
}

TEST(SkillManagerTest, ObserverFailuresDoNotTerminateSkillExecution) {
  TestProvider provider;
  SkillManagerObservers observers;
  observers.execution = [](const ExecutionSnapshot&) { throw std::runtime_error("observer"); };
  observers.emitted_signal = [](const ExecutionSnapshot&, const Signal&) {
    throw std::runtime_error("observer");
  };
  SkillManager manager(provider, {}, std::move(observers));
  const auto started =
      manager.Start("test.control", "1.0.0", {{"resource", "arm"}}, "observer-failure");
  manager.WaitForSnapshots(started.execution.execution_id, 1, std::chrono::seconds(1));
  manager.Signal(started.execution.execution_id, "control.update", nlohmann::json::object(),
                 "observer-signal");
  const auto snapshots =
      manager.WaitForSnapshots(started.execution.execution_id, 2, std::chrono::seconds(1));
  ASSERT_FALSE(snapshots.empty());
  EXPECT_EQ(snapshots.back().status, ExecutionStatus::kSucceeded);
}

TEST(SkillManagerMultiRuntimeTest, RejectsDuplicateExactSkillIdentity) {
  RoutedProvider first("test.duplicate");
  RoutedProvider second("test.duplicate");
  EXPECT_THROW((SkillManager(std::vector<SkillRuntime>{{"first", first}, {"second", second}})),
               std::invalid_argument);
}

TEST(SkillManagerMultiRuntimeTest, RoutesValidationAndExecutionToExactOwner) {
  RoutedProvider alpha("test.alpha");
  RoutedProvider beta("test.beta");
  SkillManager manager(std::vector<SkillRuntime>{{"alpha-runtime", alpha}, {"beta-runtime", beta}});
  EXPECT_EQ(manager.ListSkills().size(), 2u);
  ASSERT_NE(manager.GetSkill("test.alpha", "1.0.0"), nullptr);
  const auto started = manager.Start("test.beta", "1.0.0", nlohmann::json::object(), "beta");
  ASSERT_TRUE(WaitUntil([&] { return beta.running.load() == 1; }));
  EXPECT_EQ(alpha.validations.load(), 0);
  EXPECT_EQ(alpha.executions.load(), 0);
  EXPECT_EQ(beta.validations.load(), 1);
  EXPECT_EQ(beta.executions.load(), 1);
  manager.Signal(started.execution.execution_id, "control.update", nlohmann::json::object(),
                 "finish-beta");
  ASSERT_TRUE(WaitUntil([&] {
    const auto execution = manager.Get(started.execution.execution_id);
    return execution && IsTerminal(execution->status);
  }));
}

TEST(SkillManagerMultiRuntimeTest, CarriesTheRoutedOwnerIntoQueuedExecution) {
  RoutedProvider provider("test.queued");
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(std::vector<SkillRuntime>{{"queued-runtime", provider}}, options);
  const auto first =
      manager.Start("test.queued", "1.0.0", nlohmann::json::object(), "queued-first");
  ASSERT_TRUE(WaitUntil([&] { return provider.running.load() == 1; }));
  const auto second =
      manager.Start("test.queued", "1.0.0", nlohmann::json::object(), "queued-second");
  ASSERT_TRUE(manager.Get(second.execution.execution_id));
  EXPECT_EQ(manager.Get(second.execution.execution_id)->status, ExecutionStatus::kQueued);
  EXPECT_EQ(provider.validations.load(), 2);
  EXPECT_EQ(provider.executions.load(), 1);

  manager.Signal(first.execution.execution_id, "control.update", nlohmann::json::object(),
                 "finish-queued-first");
  ASSERT_TRUE(WaitUntil([&] { return provider.executions.load() == 2; }));
  EXPECT_EQ(provider.validations.load(), 2);
  manager.Terminate(second.execution.execution_id);
}

TEST(SkillManagerMultiRuntimeTest, RunsNonReentrantUnrelatedRuntimesConcurrently) {
  RoutedProvider alpha("test.alpha");
  RoutedProvider beta("test.beta");
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(std::vector<SkillRuntime>{{"alpha-runtime", alpha}, {"beta-runtime", beta}},
                       options);
  const auto first = manager.Start("test.alpha", "1.0.0", nlohmann::json::object(), "alpha");
  const auto second = manager.Start("test.beta", "1.0.0", nlohmann::json::object(), "beta");
  ASSERT_TRUE(WaitUntil([&] { return alpha.running.load() == 1 && beta.running.load() == 1; }));
  const auto first_snapshot = manager.Get(first.execution.execution_id);
  const auto second_snapshot = manager.Get(second.execution.execution_id);
  ASSERT_TRUE(first_snapshot);
  ASSERT_TRUE(second_snapshot);
  ASSERT_EQ(first_snapshot->claims.size(), 1u);
  ASSERT_EQ(second_snapshot->claims.size(), 1u);
  EXPECT_EQ(first_snapshot->claims.front().kind, ClaimKind::kProviderContext);
  EXPECT_EQ(first_snapshot->claims.front().value, "alpha-runtime:test.alpha@1.0.0");
  EXPECT_EQ(second_snapshot->claims.front().value, "beta-runtime:test.beta@1.0.0");
  manager.Terminate(first.execution.execution_id);
  manager.Terminate(second.execution.execution_id);
}

TEST(SkillManagerTest, SuspendedAsyncExecutionsDoNotOccupyManagerWorkers) {
  SuspendedAsyncProvider provider;
  SkillManagerOptions options;
  options.worker_count = 1;
  SkillManager manager(provider, options);
  std::vector<std::string> executions;
  for (int index = 0; index < 8; ++index) {
    executions.push_back(manager
                             .Start("test.suspended", "1.0.0", nlohmann::json::object(),
                                    "suspended-" + std::to_string(index))
                             .execution.execution_id);
  }

  ASSERT_TRUE(WaitUntil([&] { return provider.suspended() == executions.size(); }));
  for (const auto& id : executions) {
    const auto snapshot = manager.Get(id);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, ExecutionStatus::kRunning);
  }

  provider.CompleteAll();
  ASSERT_TRUE(WaitUntil([&] {
    return std::all_of(executions.begin(), executions.end(), [&](const std::string& id) {
      const auto snapshot = manager.Get(id);
      return snapshot && snapshot->status == ExecutionStatus::kSucceeded;
    });
  }));
}

TEST(SkillManagerMultiRuntimeTest, SerializesNonReentrantExecutionsWithinOneRuntime) {
  RoutedProvider alpha("test.alpha");
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(std::vector<SkillRuntime>{{"alpha-runtime", alpha}}, options);
  const auto first = manager.Start("test.alpha", "1.0.0", nlohmann::json::object(), "first");
  ASSERT_TRUE(WaitUntil([&] { return alpha.running.load() == 1; }));
  const auto second = manager.Start("test.alpha", "1.0.0", nlohmann::json::object(), "second");
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(manager.Get(second.execution.execution_id));
  EXPECT_EQ(manager.Get(second.execution.execution_id)->status, ExecutionStatus::kQueued);
  EXPECT_EQ(manager.Get(second.execution.execution_id)->queue_reason, "claim.conflict");
  manager.Signal(first.execution.execution_id, "control.update", nlohmann::json::object(),
                 "finish-first");
  ASSERT_TRUE(WaitUntil([&] { return alpha.executions.load() == 2; }));
  manager.Terminate(second.execution.execution_id);
}

TEST(SkillManagerMultiRuntimeTest, RoutesSignalsAndCancellationToOwningExecutionRuntime) {
  RoutedProvider alpha("test.alpha");
  RoutedProvider beta("test.beta");
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(std::vector<SkillRuntime>{{"alpha-runtime", alpha}, {"beta-runtime", beta}},
                       options);
  const auto first = manager.Start("test.alpha", "1.0.0", nlohmann::json::object(), "alpha");
  const auto second = manager.Start("test.beta", "1.0.0", nlohmann::json::object(), "beta");
  ASSERT_TRUE(WaitUntil([&] { return alpha.running.load() == 1 && beta.running.load() == 1; }));
  manager.Signal(second.execution.execution_id, "control.update", nlohmann::json::object(),
                 "beta-signal");
  ASSERT_TRUE(WaitUntil([&] { return beta.signals.load() == 1; }));
  EXPECT_EQ(alpha.signals.load(), 0);
  EXPECT_TRUE(manager.Terminate(first.execution.execution_id));
  ASSERT_TRUE(WaitUntil([&] { return alpha.cancellations.load() == 1; }));
  EXPECT_EQ(beta.cancellations.load(), 0);
}

TEST(SkillManagerTest, RetainsProcessJournalAndAccounting) {
  JournalProvider provider;
  SkillManagerOptions options;
  options.worker_count = 1;
  SkillManager manager(provider, options);
  const auto started =
      manager.Start("test.record", "1.0.0", nlohmann::json::object(), "record-request");
  ASSERT_TRUE(WaitUntil([&] {
    const auto execution = manager.Get(started.execution.execution_id);
    return execution && IsTerminal(execution->status);
  }));
  const auto snapshot = manager.Get(started.execution.execution_id);
  ASSERT_TRUE(snapshot);
  EXPECT_TRUE(snapshot->created_at.has_value());
  EXPECT_TRUE(snapshot->started_at.has_value());
  EXPECT_TRUE(snapshot->finished_at.has_value());
  EXPECT_FALSE(snapshot->current_activity.has_value());
  EXPECT_EQ(snapshot->message, "record completed");
  EXPECT_EQ(snapshot->accounting.resource_reads, 2U);
  EXPECT_EQ(snapshot->accounting.primitive_calls, 1U);
  EXPECT_EQ(snapshot->accounting.capability_duration, std::chrono::milliseconds(12));
  EXPECT_EQ(snapshot->accounting.diagnostic_logs, 1U);
  EXPECT_EQ(snapshot->accounting.diagnostic_logs_dropped, 1U);
  EXPECT_EQ(snapshot->accounting.checkpoints, 1U);
  EXPECT_EQ(snapshot->accounting.activity_events, 2U);

  const auto events = manager.WaitForJournal(started.execution.execution_id, 0,
                                                     std::chrono::milliseconds(0));
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const JournalEntry& event) {
    return event.kind == JournalEntry::Kind::kRecord &&
           event.record.kind == JournalKind::kCheckpoint && event.record.name == "work.started";
  }));
  EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const JournalEntry& event) {
    return event.kind == JournalEntry::Kind::kRecord &&
           event.record.kind == JournalKind::kSuppression;
  }));
}

TEST(SkillManagerTest, MonitorsActiveExecutionsWithReplayAndTerminalRemoval) {
  RoutedProvider provider("test.monitor", true);
  SkillManager manager(std::vector<SkillRuntime>{{"monitor-runtime", provider}});
  const auto started =
      manager.Start("test.monitor", "1.0.0", nlohmann::json::object(), "monitor-start");
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(started.execution.execution_id);
    return snapshot && snapshot->status == ExecutionStatus::kRunning;
  }));

  const auto initial = manager.WaitForMonitor({}, 0, std::chrono::milliseconds(0));
  ASSERT_EQ(initial.size(), 1u);
  EXPECT_TRUE(initial.front().reset);
  ASSERT_EQ(initial.front().upserted.size(), 1u);
  EXPECT_EQ(initial.front().upserted.front().runtime_id, "monitor-runtime");
  std::uint64_t cursor = initial.front().sequence;

  ASSERT_TRUE(manager.Terminate(started.execution.execution_id));
  bool removed = false;
  for (int attempt = 0; attempt < 20 && !removed; ++attempt) {
    const auto updates =
        manager.WaitForMonitor({}, cursor, std::chrono::milliseconds(100));
    for (const auto& update : updates) {
      cursor = update.sequence;
      removed |= std::find(update.removed_execution_ids.begin(),
                           update.removed_execution_ids.end(),
                           started.execution.execution_id) != update.removed_execution_ids.end();
    }
  }
  EXPECT_TRUE(removed);
  EXPECT_THROW(manager.WaitForMonitor({}, cursor + 100, std::chrono::milliseconds(0)),
               std::invalid_argument);
}

TEST(SkillManagerTest, BulkTerminationUsesAnIdempotentAtomicTargetSet) {
  RoutedProvider provider("test.pkill", true);
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(std::vector<SkillRuntime>{{"pkill-runtime", provider}}, options);
  const auto first = manager.Start("test.pkill", "1.0.0", nlohmann::json::object(), "pkill-1");
  const auto second = manager.Start("test.pkill", "1.0.0", nlohmann::json::object(), "pkill-2");
  ASSERT_TRUE(WaitUntil([&] { return provider.running.load() == 2; }));

  const auto terminated = manager.TerminateMany("test.pkill", "1.0.0", "bulk-terminate");
  EXPECT_EQ(terminated.status, TerminateManyResult::Status::kAccepted);
  EXPECT_EQ(terminated.execution_ids,
            (std::vector<std::string>{std::min(first.execution.execution_id,
                                               second.execution.execution_id),
                                      std::max(first.execution.execution_id,
                                               second.execution.execution_id)}));
  const auto replay = manager.TerminateMany("test.pkill", "1.0.0", "bulk-terminate");
  EXPECT_EQ(replay.status, TerminateManyResult::Status::kExisting);
  EXPECT_EQ(replay.execution_ids, terminated.execution_ids);
  const auto conflict = manager.TerminateMany("test.pkill", "2.0.0", "bulk-terminate");
  EXPECT_EQ(conflict.status, TerminateManyResult::Status::kConflict);
  ASSERT_TRUE(WaitUntil([&] {
    return IsTerminal(manager.Get(first.execution.execution_id)->status) &&
           IsTerminal(manager.Get(second.execution.execution_id)->status);
  }));
}

TEST(SkillManagerTest, SupervisesChildProcessesAndTypedMailboxes) {
  ProcessProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("test.parent", "1.0.0", nlohmann::json::object(), "parent-request");
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(parent.execution.execution_id);
    return snapshot && IsTerminal(snapshot->status);
  }));
  const auto parent_snapshot = manager.Get(parent.execution.execution_id);
  ASSERT_TRUE(parent_snapshot.has_value());
  EXPECT_EQ(parent_snapshot->status, ExecutionStatus::kSucceeded);
  EXPECT_EQ(parent_snapshot->result, nlohmann::json({{"value", "ping"}}));
  const auto executions = manager.ListExecutions();
  const auto child = std::find_if(executions.begin(), executions.end(), [&](const auto& snapshot) {
    return snapshot.parent_execution_id == parent.execution.execution_id;
  });
  ASSERT_NE(child, executions.end());
  EXPECT_EQ(child->skill_name, "test.child");
  EXPECT_TRUE(IsTerminal(child->status));
}

TEST(SkillManagerTest, QueuedCancellationIsCompleteAndPreventsChildExecution) {
  PassiveProcessProvider provider;
  SkillManagerOptions options;
  options.worker_count = 1;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "parent");
  ASSERT_TRUE(WaitUntil([&] { return provider.executions.load() == 1; }));
  const auto child = manager.SpawnChild(parent.execution.execution_id, "passive.child", "1.0.0",
                                        nlohmann::json::object(), "child");

  ASSERT_TRUE(manager.Terminate(parent.execution.execution_id));
  ASSERT_TRUE(WaitUntil([&] {
    const auto snapshot = manager.Get(child.execution.execution_id);
    return snapshot && IsTerminal(snapshot->status);
  }));
  const auto snapshot = manager.Get(child.execution.execution_id);
  ASSERT_TRUE(snapshot);
  EXPECT_EQ(snapshot->status, ExecutionStatus::kCancelled);
  EXPECT_TRUE(snapshot->finished_at.has_value());
  EXPECT_GE(snapshot->total_duration, std::chrono::milliseconds(0));
  EXPECT_EQ(snapshot->queue_duration, snapshot->total_duration);
  EXPECT_EQ(provider.executions.load(), 1);
}

TEST(SkillManagerTest, ChildStartIdempotencyIncludesParentIdentity) {
  PassiveProcessProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  SkillManager manager(provider, options);
  const auto first = manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "first");
  const auto second = manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "second");
  ASSERT_TRUE(WaitUntil([&] { return provider.executions.load() == 2; }));
  const auto child = manager.SpawnChild(first.execution.execution_id, "passive.child", "1.0.0",
                                        nlohmann::json::object(), "same-child-request");
  EXPECT_EQ(manager.SpawnChild(first.execution.execution_id, "passive.child", "1.0.0",
                               nlohmann::json::object(), "same-child-request").status,
            StartResult::Status::kExisting);
  EXPECT_EQ(manager.SpawnChild(second.execution.execution_id, "passive.child", "1.0.0",
                               nlohmann::json::object(), "same-child-request").status,
            StartResult::Status::kConflict);
  manager.Terminate(first.execution.execution_id);
  manager.Terminate(second.execution.execution_id);
  manager.Terminate(child.execution.execution_id);
}

TEST(SkillManagerTest, PrunesExpiredIpcAndRejectsTerminalSender) {
  PassiveProcessProvider provider;
  eos::lib::ManualClock clock(eos::lib::MonotonicTime(std::chrono::seconds(1)),
                              eos::lib::WallTime(std::chrono::seconds(1)));
  SkillManagerOptions options;
  options.clock = &clock;
  options.worker_count = 2;
  options.max_pending_signals = 1;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "ipc-parent");
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(parent.execution.execution_id).has_value(); }));
  const auto parent_hooks = *provider.Hooks(parent.execution.execution_id);
  const ProcessHandle child_handle = parent_hooks.spawn(
      "passive.child", "1.0.0", nlohmann::json::object(), "ipc-child", std::chrono::seconds(5));
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(child_handle.execution_id).has_value(); }));
  const auto child_hooks = *provider.Hooks(child_handle.execution_id);
  ASSERT_TRUE(child_hooks.parent_process);
  EXPECT_TRUE(manager.SendMessage(parent.execution.execution_id, child_handle, "process.test",
                                  nlohmann::json::object(), "expired", std::chrono::milliseconds(1)));
  clock.Advance(std::chrono::milliseconds(2));
  EXPECT_TRUE(manager.SendMessage(parent.execution.execution_id, child_handle, "process.test",
                                  nlohmann::json::object(), "live", std::chrono::seconds(1)));
  const auto received = manager.ReceiveMessage(child_handle.execution_id, "process.test",
                                                 std::chrono::milliseconds(0));
  ASSERT_TRUE(received);
  EXPECT_EQ(received->request_id, "live");

  ProcessHandle stale_child = child_handle;
  ++stale_child.generation;
  EXPECT_THROW(manager.SendMessage(parent.execution.execution_id, stale_child, "process.test",
                                   nlohmann::json::object(), "stale-send",
                                   std::chrono::seconds(1)),
               std::invalid_argument);
  EXPECT_THROW(manager.AwaitExit(parent.execution.execution_id, stale_child,
                                 std::chrono::milliseconds(0)),
               std::invalid_argument);
  EXPECT_THROW(manager.TerminateChild(parent.execution.execution_id, stale_child),
               std::invalid_argument);

  ASSERT_TRUE(manager.Terminate(child_handle.execution_id));
  ASSERT_TRUE(WaitUntil([&] {
    return IsTerminal(manager.Get(child_handle.execution_id)->status);
  }));
  EXPECT_THROW(manager.SendMessage(child_handle.execution_id, *child_hooks.parent_process,
                                    "process.test",
                                    nlohmann::json::object(), "dead-sender",
                                    std::chrono::seconds(1)),
               std::logic_error);
  manager.Terminate(parent.execution.execution_id);
}

TEST(SkillManagerTest, BoundsIpcRequestTrackingIndependentlyOfConsumedMailboxMessages) {
  PassiveProcessProvider provider;
  SkillManagerOptions options;
  options.worker_count = 2;
  options.max_pending_signals = 2;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "tracking-parent");
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(parent.execution.execution_id).has_value(); }));
  const auto parent_hooks = *provider.Hooks(parent.execution.execution_id);
  const ProcessHandle child_handle = parent_hooks.spawn(
      "passive.child", "1.0.0", nlohmann::json::object(), "tracking-child",
      std::chrono::seconds(5));
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(child_handle.execution_id).has_value(); }));
  for (const std::string request : {"tracked-1", "tracked-2", "tracked-3"}) {
    EXPECT_TRUE(manager.SendMessage(parent.execution.execution_id, child_handle, "process.test",
                                    nlohmann::json::object(), request, std::chrono::seconds(5)));
    EXPECT_TRUE(manager.ReceiveMessage(child_handle.execution_id, "process.test",
                                       std::chrono::milliseconds(0)));
  }
  EXPECT_TRUE(manager.SendMessage(parent.execution.execution_id, child_handle, "process.test",
                                  nlohmann::json::object(), "tracked-1",
                                  std::chrono::seconds(5)));
  EXPECT_FALSE(manager.SendMessage(parent.execution.execution_id, child_handle, "process.test",
                                   nlohmann::json::object(), "tracked-3",
                                   std::chrono::seconds(5)));
  manager.Terminate(parent.execution.execution_id);
}

TEST(SkillManagerTest, ScopesSameCorrelationRepliesToExpectedPeer) {
  PassiveProcessProvider provider;
  SkillManagerOptions options;
  options.worker_count = 3;
  SkillManager manager(provider, options);
  const auto parent =
      manager.Start("passive.parent", "1.0.0", nlohmann::json::object(), "reply-parent");
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(parent.execution.execution_id).has_value(); }));
  const auto parent_hooks = *provider.Hooks(parent.execution.execution_id);
  const ProcessHandle first_child = parent_hooks.spawn(
      "passive.child", "1.0.0", nlohmann::json::object(), "reply-child-1",
      std::chrono::seconds(5));
  const ProcessHandle second_child = parent_hooks.spawn(
      "passive.child", "1.0.0", nlohmann::json::object(), "reply-child-2",
      std::chrono::seconds(5));
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(first_child.execution_id).has_value(); }));
  ASSERT_TRUE(WaitUntil([&] { return provider.Hooks(second_child.execution_id).has_value(); }));
  const auto first_hooks = *provider.Hooks(first_child.execution_id);
  const auto second_hooks = *provider.Hooks(second_child.execution_id);
  ASSERT_TRUE(first_hooks.parent_process);
  ASSERT_TRUE(second_hooks.parent_process);
  ASSERT_TRUE(manager.SendMessage(second_child.execution_id, *second_hooks.parent_process,
                                  "process.test", {{"child", 2}}, "same-request",
                                  std::chrono::seconds(1)));
  ASSERT_TRUE(manager.SendMessage(first_child.execution_id, *first_hooks.parent_process,
                                  "process.test", {{"child", 1}}, "same-request",
                                  std::chrono::seconds(1)));

  std::optional<IpcMessage> first;
  std::optional<IpcMessage> second;
  std::thread first_waiter([&] {
    first = manager.ReceiveMessage(parent.execution.execution_id, "process.test",
                                   std::chrono::seconds(1), "same-request",
                                   first_child.execution_id);
  });
  std::thread second_waiter([&] {
    second = manager.ReceiveMessage(parent.execution.execution_id, "process.test",
                                    std::chrono::seconds(1), "same-request",
                                    second_child.execution_id);
  });
  first_waiter.join();
  second_waiter.join();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->sender_execution_id, first_child.execution_id);
  EXPECT_EQ(first->payload.at("child"), 1);
  EXPECT_EQ(second->sender_execution_id, second_child.execution_id);
  EXPECT_EQ(second->payload.at("child"), 2);
  manager.Terminate(parent.execution.execution_id);
}
}  // namespace
}  // namespace eos::runtime::skills

#include "src/eos/runtime/actionlist/manager.h"
#include "src/eos/runtime/actionlist/journal.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>

namespace eos::runtime::actionlist {
namespace {

nlohmann::json SkillAction(std::string id, nlohmann::json dependencies = nlohmann::json::array(),
                           std::optional<nlohmann::json> when = std::nullopt) {
  nlohmann::json action{{"action_id", std::move(id)},
                        {"skill", {{"name", "test.skill"}, {"version", "1.0.0"}}},
                        {"parameters", nlohmann::json::object()},
                        {"depends_on", std::move(dependencies)}};
  if (when) action["when"] = std::move(*when);
  return action;
}

nlohmann::json PrimitiveAction(std::string id, std::string name = "test.primitive") {
  return {
      {"action_id", std::move(id)},
      {"primitive", {{"name", std::move(name)}, {"version", "1.0.0"}, {"device_id", "device-1"}}},
      {"parameters", nlohmann::json::object()}};
}

nlohmann::json NoopAction(std::string id) {
  return {{"action_id", std::move(id)},
          {"noop", nlohmann::json::object()},
          {"parameters", nlohmann::json::object()}};
}

nlohmann::json Binding(std::string source, std::string source_pointer,
                       std::string destination_pointer) {
  return {{"source_action_id", std::move(source)},
          {"source_output_pointer", std::move(source_pointer)},
          {"destination_parameter_pointer", std::move(destination_pointer)}};
}

nlohmann::json Action(const nlohmann::json& list, const std::string& id) {
  for (const auto& action : list.at("actions"))
    if (action.at("action_id") == id) return action;
  return nullptr;
}

class FakeSkills {
 public:
  SkillRunner Runner() {
    return {[this](const std::string& name, const std::string& version, nlohmann::json arguments,
                   const std::string& request_id) {
              skills::ExecutionSnapshot execution;
              execution.execution_id = "execution-" + std::to_string(++sequence_);
              execution.request_id = request_id;
              execution.skill_name = name;
              execution.skill_version = version;
              execution.status = skills::ExecutionStatus::kQueued;
              executions_.push_back(execution);
              arguments_.push_back(std::move(arguments));
              return skills::StartResult{skills::StartResult::Status::kStarted, execution};
            },
            [this](const std::string& id) {
              cancelled_.push_back(id);
              return true;
            },
            [this] { return executions_; }};
  }

  std::vector<skills::ExecutionSnapshot> executions_;
  std::vector<nlohmann::json> arguments_;
  std::vector<std::string> cancelled_;

 private:
  std::uint64_t sequence_{0};
};

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-actionlist-manager-test-" + std::to_string(++sequence_))) {
    std::filesystem::remove_all(path);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }

  static inline std::atomic<unsigned> sequence_{0};
  std::filesystem::path path;
};

nlohmann::json WaitFor(Manager& manager, const std::string& id,
                       const std::function<bool(const nlohmann::json&)>& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  nlohmann::json latest;
  do {
    if (auto list = manager.Get(id)) {
      latest = *list;
      if (predicate(latest)) return latest;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  ADD_FAILURE() << latest.dump();
  return latest;
}

TEST(ActionListManagerTest, RejectsMalformedGraphsAndCycles) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  EXPECT_THROW(manager.Submit(nlohmann::json::object()), std::invalid_argument);
  EXPECT_THROW(manager.Submit({{"workflow_id", "empty"}, {"actions", nlohmann::json::array()}}),
               std::invalid_argument);
  EXPECT_THROW(manager.Submit({{"workflow_id", "duplicate"},
                               {"actions", {SkillAction("same"), SkillAction("same")}}}),
               std::invalid_argument);
  EXPECT_THROW(manager.Submit({{"workflow_id", "missing"},
                               {"actions", {SkillAction("a", {"does-not-exist"})}}}),
               std::invalid_argument);
  EXPECT_THROW(manager.Submit({{"workflow_id", "self"}, {"actions", {SkillAction("a", {"a"})}}}),
               std::invalid_argument);
  EXPECT_THROW(manager.Submit({{"workflow_id", "cycle"},
                               {"actions", {SkillAction("a", {"b"}), SkillAction("b", {"a"})}}}),
               std::invalid_argument);
  EXPECT_THROW(
      manager.Submit(
          {{"workflow_id", "two-targets"},
           {"actions",
            {{{"action_id", "a"},
              {"skill", {{"name", "test.skill"}, {"version", "1.0.0"}}},
              {"primitive", {{"name", "test.primitive"}, {"version", "1.0.0"}, {"device_id", "d"}}},
              {"parameters", nlohmann::json::object()}}}}}),
      std::invalid_argument);
  EXPECT_TRUE(manager.List().empty());
}

TEST(ActionListManagerTest, AdvancesDagFromObservedSkillExecutions) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  manager.Submit(
      {{"workflow_id", "dag"},
       {"actions",
        {SkillAction("left"), SkillAction("right"), SkillAction("join", {"left", "right"})}}});
  auto running = manager.Get("dag").value();
  EXPECT_EQ(Action(running, "join").at("status"), "blocked");
  for (auto execution : skills.executions_) {
    execution.status = skills::ExecutionStatus::kSucceeded;
    execution.result = nlohmann::json::object();
    manager.ObserveExecution(execution);
  }
  ASSERT_EQ(skills.executions_.size(), 3u);
  auto join = skills.executions_.back();
  join.status = skills::ExecutionStatus::kSucceeded;
  manager.ObserveExecution(join);
  EXPECT_EQ(manager.Get("dag")->at("status"), "succeeded");
}

TEST(ActionListManagerTest, ResolvesSkillOutputsIntoNestedParametersAndAddsDependency) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto consume = SkillAction("consume");
  consume["parameters"] = {{"items", nlohmann::json::array({0, nullptr, "template"})},
                           {"nested", {{"value", false}}},
                           {"escaped", nullptr}};
  consume["output_bindings"] = {
      Binding("produce", "/values/1", "/items/0"),
      Binding("produce", "/nothing", "/items/1"),
      Binding("produce", "/values/0/name", "/nested/value"),
      Binding("produce", "/a~1b~0c", "/escaped")};
  manager.Submit({{"workflow_id", "skill-bindings"},
                  {"actions", {SkillAction("produce"), consume}}});

  ASSERT_EQ(skills.executions_.size(), 1u);
  auto produced = skills.executions_.front();
  produced.status = skills::ExecutionStatus::kSucceeded;
  produced.result = {
      {"values", {{{"name", "Ada"}}, 42}}, {"nothing", nullptr}, {"a/b~c", "escaped"}};
  manager.ObserveExecution(produced);

  ASSERT_EQ(skills.executions_.size(), 2u);
  EXPECT_EQ(skills.arguments_[1],
            (nlohmann::json{{"items", nlohmann::json::array({42, nullptr, "template"})},
                            {"nested", {{"value", "Ada"}}},
                            {"escaped", "escaped"}}));
  const auto item = Action(*manager.Get("skill-bindings"), "consume");
  EXPECT_EQ(item.at("parameters"), consume.at("parameters"));
  EXPECT_EQ(item.at("resolved_parameters"), skills.arguments_[1]);
  EXPECT_EQ(item.at("depends_on"), nlohmann::json::array({"produce"}));
  EXPECT_EQ(Action(*manager.Get("skill-bindings"), "produce").at("output"), produced.result);
}

TEST(ActionListManagerTest, LegacyDefinitionDispatchesItsUnchangedParameterTemplate) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto action = SkillAction("legacy");
  action["parameters"] = {{"nested", {{"value", 9}}}, {"enabled", true}};
  const auto submitted =
      manager.Submit({{"workflow_id", "legacy-definition"}, {"actions", {action}}});

  ASSERT_EQ(skills.arguments_.size(), 1u);
  EXPECT_EQ(skills.arguments_.front(), action.at("parameters"));
  const auto item = Action(submitted, "legacy");
  EXPECT_EQ(item.at("parameters"), action.at("parameters"));
  EXPECT_EQ(item.at("resolved_parameters"), action.at("parameters"));
  EXPECT_FALSE(item.contains("output_bindings"));
}

TEST(ActionListManagerTest, ResolvesPrimitiveNormalizedOutputIntoLaterPrimitive) {
  FakeSkills skills;
  std::mutex captured_mutex;
  nlohmann::json captured;
  PrimitiveRunner primitive;
  primitive.execute = [&](const std::string& name, const std::string&, const std::string&,
                          nlohmann::json arguments, const std::string&,
                          const std::function<bool()>&) {
    if (name == "produce")
      return nlohmann::json{{"accepted", true},
                            {"result", {{"status", "succeeded"},
                                        {"payload", {{"coordinates", {3, 5}}}}}}};
    std::lock_guard<std::mutex> lock(captured_mutex);
    captured = std::move(arguments);
    return nlohmann::json{{"result", {{"status", "succeeded"}}}};
  };
  Manager manager(skills.Runner(), std::move(primitive), {1});
  auto consume = PrimitiveAction("consume", "consume");
  consume["parameters"] = {{"target", {0, 0}}};
  consume["output_bindings"] = {
      Binding("produce", "/payload/coordinates", "/target")};
  manager.Submit({{"workflow_id", "primitive-bindings"},
                  {"actions", {PrimitiveAction("produce", "produce"), consume}}});
  const auto terminal = WaitFor(manager, "primitive-bindings",
                                [](const auto& list) { return list.at("status") == "succeeded"; });
  std::lock_guard<std::mutex> lock(captured_mutex);
  EXPECT_EQ(captured, (nlohmann::json{{"target", {3, 5}}}));
  EXPECT_EQ(Action(terminal, "produce").at("result").at("accepted"), true);
  EXPECT_EQ(Action(terminal, "produce").at("output").at("payload").at("coordinates"),
            nlohmann::json({3, 5}));
}

TEST(ActionListManagerTest, RejectsInvalidBindingDefinitionsDeterministically) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto destination = SkillAction("destination");
  destination["parameters"] = {{"value", nullptr}, {"nested", {{"value", nullptr}}}};

  auto missing = destination;
  missing["output_bindings"] = {Binding("absent", "", "/value")};
  EXPECT_THROW(manager.Submit({{"workflow_id", "missing-binding-source"},
                               {"actions", {SkillAction("source"), missing}}}),
               std::invalid_argument);

  auto future = SkillAction("first");
  future["parameters"] = {{"value", nullptr}};
  future["output_bindings"] = {Binding("later", "", "/value")};
  EXPECT_THROW(manager.Submit({{"workflow_id", "future-binding-source"},
                               {"actions", {future, SkillAction("later")}}}),
               std::invalid_argument);

  auto invalid = destination;
  invalid["output_bindings"] = {Binding("source", "/bad~2escape", "/value")};
  EXPECT_THROW(manager.Submit({{"workflow_id", "invalid-binding-pointer"},
                               {"actions", {SkillAction("source"), invalid}}}),
               std::invalid_argument);

  auto missing_destination = destination;
  missing_destination["output_bindings"] = {Binding("source", "", "/absent")};
  EXPECT_THROW(manager.Submit({{"workflow_id", "missing-binding-destination"},
                               {"actions", {SkillAction("source"), missing_destination}}}),
               std::invalid_argument);

  auto conflicting = destination;
  conflicting["output_bindings"] = {Binding("source", "", "/nested"),
                                     Binding("source", "", "/nested/value")};
  EXPECT_THROW(manager.Submit({{"workflow_id", "conflicting-bindings"},
                               {"actions", {SkillAction("source"), conflicting}}}),
               std::invalid_argument);
}

TEST(ActionListManagerTest, RejectsMissingOutputPathAndDoesNotDispatchAfterFailedSource) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto consume = SkillAction("consume");
  consume["parameters"] = {{"value", nullptr}};
  consume["output_bindings"] = {Binding("produce", "/missing", "/value")};
  manager.Submit({{"workflow_id", "missing-output"},
                  {"actions", {SkillAction("produce"), consume}}});
  auto produced = skills.executions_.front();
  produced.status = skills::ExecutionStatus::kSucceeded;
  produced.result = {{"present", true}};
  manager.ObserveExecution(produced);
  EXPECT_EQ(Action(*manager.Get("missing-output"), "consume").at("status"), "rejected");
  EXPECT_EQ(skills.executions_.size(), 1u);

  FakeSkills failed_skills;
  Manager failed_manager(failed_skills.Runner());
  failed_manager.Submit({{"workflow_id", "failed-source"},
                         {"actions", {SkillAction("produce"), consume}}});
  auto failed = failed_skills.executions_.front();
  failed.status = skills::ExecutionStatus::kFailed;
  failed.result = {{"present", true}};
  failed_manager.ObserveExecution(failed);
  EXPECT_EQ(Action(*failed_manager.Get("failed-source"), "consume").at("status"), "skipped");
  EXPECT_FALSE(Action(*failed_manager.Get("failed-source"), "consume")
                   .contains("resolved_parameters"));
  EXPECT_EQ(failed_skills.executions_.size(), 1u);
}

TEST(ActionListManagerTest, ContinuesAfterToleratedFailureWithoutUsingFailedOutput) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto tolerated = SkillAction("tolerated");
  tolerated["continue_on_failure"] = true;
  auto bound = SkillAction("bound");
  bound["parameters"] = {{"value", nullptr}};
  bound["output_bindings"] = {Binding("tolerated", "/value", "/value")};
  manager.Submit({{"workflow_id", "tolerated-failure"},
                  {"actions", {tolerated, SkillAction("next", {"tolerated"}), bound}}});

  auto failed = skills.executions_.front();
  failed.status = skills::ExecutionStatus::kFailed;
  failed.reason_code = "expected.failure";
  manager.ObserveExecution(failed);

  ASSERT_EQ(skills.executions_.size(), 2u);
  auto next = skills.executions_.back();
  next.status = skills::ExecutionStatus::kSucceeded;
  next.result = nlohmann::json::object();
  manager.ObserveExecution(next);

  const auto complete = manager.Get("tolerated-failure").value();
  EXPECT_EQ(Action(complete, "tolerated").at("status"), "failed");
  EXPECT_EQ(Action(complete, "tolerated").at("reason"), "expected.failure");
  EXPECT_EQ(Action(complete, "next").at("status"), "succeeded");
  EXPECT_EQ(Action(complete, "bound").at("status"), "skipped");
  EXPECT_EQ(Action(complete, "bound").at("reason"),
            "dependency_output_unavailable:tolerated");
  EXPECT_EQ(complete.at("status"), "succeeded");
}

TEST(ActionListManagerTest, NonToleratedFailureStillFailsAndBlocksNormalWork) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  manager.Submit({{"workflow_id", "fatal-failure"},
                  {"actions", {SkillAction("first"), SkillAction("next", {"first"})}}});
  auto failed = skills.executions_.front();
  failed.status = skills::ExecutionStatus::kFailed;
  manager.ObserveExecution(failed);

  const auto complete = manager.Get("fatal-failure").value();
  EXPECT_EQ(Action(complete, "next").at("status"), "skipped");
  EXPECT_EQ(Action(complete, "next").at("reason"), "workflow_failed");
  EXPECT_EQ(complete.at("status"), "failed");
  EXPECT_EQ(skills.executions_.size(), 1u);
}

TEST(ActionListManagerTest, SynchronousStartObservationsDoNotDeadlockOrGetOverwritten) {
  Manager* manager = nullptr;
  std::vector<skills::ExecutionSnapshot> executions;
  bool inside_start = false;
  SkillRunner runner;
  runner.start = [&](const std::string& name, const std::string& version, nlohmann::json,
                     const std::string& request_id) {
    EXPECT_FALSE(inside_start);
    inside_start = true;
    skills::ExecutionSnapshot queued;
    queued.execution_id = "synchronous-execution-" + std::to_string(executions.size() + 1);
    queued.request_id = request_id;
    queued.skill_name = name;
    queued.skill_version = version;
    queued.status = skills::ExecutionStatus::kQueued;
    executions.push_back(queued);
    auto running = queued;
    running.status = skills::ExecutionStatus::kRunning;
    manager->ObserveExecution(running);
    auto succeeded = running;
    succeeded.status = skills::ExecutionStatus::kSucceeded;
    manager->ObserveExecution(succeeded);
    executions.back() = succeeded;
    inside_start = false;
    return skills::StartResult{skills::StartResult::Status::kStarted, queued};
  };
  runner.cancel = [](const std::string&) { return true; };
  runner.list_executions = [&] { return executions; };
  Manager instance(std::move(runner));
  manager = &instance;

  const auto submitted =
      instance.Submit({{"workflow_id", "synchronous"},
                       {"actions", {SkillAction("first"), SkillAction("second", {"first"})}}});
  EXPECT_EQ(submitted.at("status"), "succeeded");
  EXPECT_EQ(Action(submitted, "first").at("status"), "succeeded");
  EXPECT_EQ(Action(submitted, "first").at("execution_id"), "synchronous-execution-1");
  EXPECT_EQ(Action(submitted, "second").at("status"), "succeeded");
  EXPECT_EQ(Action(submitted, "second").at("execution_id"), "synchronous-execution-2");
}

TEST(ActionListManagerTest, ThrowingSkillCancelDoesNotStrandReplacementDispatch) {
  std::vector<skills::ExecutionSnapshot> executions;
  std::vector<std::string> cancel_attempts;
  SkillRunner runner;
  runner.start = [&](const std::string& name, const std::string& version, nlohmann::json,
                     const std::string& request_id) {
    skills::ExecutionSnapshot execution;
    execution.execution_id = "execution-" + std::to_string(executions.size() + 1);
    execution.request_id = request_id;
    execution.skill_name = name;
    execution.skill_version = version;
    executions.push_back(execution);
    return skills::StartResult{skills::StartResult::Status::kStarted, execution};
  };
  runner.cancel = [&](const std::string& execution_id) {
    cancel_attempts.push_back(execution_id);
    if (cancel_attempts.size() == 1) throw std::runtime_error("runtime unavailable");
    return true;
  };
  runner.list_executions = [&] { return executions; };
  Manager manager(std::move(runner));
  manager.Submit(
      {{"workflow_id", "old"}, {"actions", {SkillAction("left"), SkillAction("right")}}});

  EXPECT_NO_THROW(manager.Submit({{"workflow_id", "new"},
                                  {"supersedes_workflow_ids", {"old"}},
                                  {"actions", {SkillAction("replacement")}}}));
  ASSERT_EQ(cancel_attempts.size(), 2u);
  ASSERT_EQ(executions.size(), 3u);
  const auto replacement = manager.Get("new").value();
  EXPECT_EQ(Action(replacement, "replacement").at("status"), "queued");
  EXPECT_EQ(Action(replacement, "replacement").at("execution_id"), "execution-3");

  for (std::size_t index = 0; index < 2; ++index) {
    auto cancelled = executions[index];
    cancelled.status = skills::ExecutionStatus::kCancelled;
    manager.ObserveExecution(cancelled);
  }
  EXPECT_EQ(manager.Get("old")->at("status"), "superseded");
  EXPECT_EQ(Action(*manager.Get("new"), "replacement").at("execution_id"), "execution-3");
}

TEST(ActionListManagerTest, FailedSkillCancelTerminatesSupersededAction) {
  FakeSkills skills;
  auto runner = skills.Runner();
  runner.cancel = [](const std::string&) { return false; };
  Manager manager(std::move(runner));
  manager.Submit({{"workflow_id", "old"}, {"actions", {SkillAction("active")}}});
  manager.Submit({{"workflow_id", "new"},
                  {"supersedes_workflow_ids", {"old"}},
                  {"actions", {SkillAction("replacement")}}});

  const auto old = manager.Get("old").value();
  EXPECT_EQ(old.at("status"), "superseded");
  EXPECT_EQ(Action(old, "active").at("status"), "failed");
  EXPECT_EQ(Action(old, "active").at("reason"), "cancellation.request_failed");
  EXPECT_EQ(Action(*manager.Get("new"), "replacement").at("status"), "queued");
}

TEST(ActionListManagerTest, OrdersFactsAndReevaluatesConditions) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  const auto old = std::chrono::system_clock::now() - std::chrono::seconds(1);
  EXPECT_TRUE(manager.UpdateFact("gate", {true, "sensor", 1, old, old}));
  manager.Submit(
      {{"workflow_id", "facts"},
       {"actions",
        {SkillAction("conditional", nlohmann::json::array(),
                     nlohmann::json{{"fact", "gate"}, {"equals", true}, {"max_age_ms", 50}})}}});
  EXPECT_EQ(Action(*manager.Get("facts"), "conditional").at("status"), "blocked");
  EXPECT_FALSE(
      manager.UpdateFact("gate", {true, "sensor", 1, std::chrono::system_clock::now(), {}}));
  EXPECT_FALSE(manager.UpdateFact("gate", {true, "sensor", 2, old - std::chrono::seconds(1), {}}));
  EXPECT_TRUE(
      manager.UpdateFact("gate", {false, "sensor", 2, std::chrono::system_clock::now(), {}}));
  EXPECT_EQ(Action(*manager.Get("facts"), "conditional").at("status"), "blocked");
  EXPECT_TRUE(
      manager.UpdateFact("gate", {true, "sensor", 3, std::chrono::system_clock::now(), {}}));
  EXPECT_EQ(Action(*manager.Get("facts"), "conditional").at("status"), "queued");
}

TEST(ActionListManagerTest, RejectsFutureFactsAndRetainsPerSourceSequenceOrdering) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  const auto now = std::chrono::system_clock::now();
  EXPECT_THROW(manager.UpdateFact("gate", {true, "a", 1, now + std::chrono::hours(1), now}),
               std::invalid_argument);
  EXPECT_TRUE(manager.UpdateFact("gate", {1, "a", 10, now, now}));
  EXPECT_TRUE(manager.UpdateFact("gate", {2, "b", 1, now, now + std::chrono::milliseconds(1)}));
  EXPECT_FALSE(manager.UpdateFact("gate", {3, "a", 9, now, now + std::chrono::milliseconds(2)}));
  EXPECT_EQ(manager.Snapshot().facts.at("gate").value, 2);
}

TEST(ActionListManagerTest, BoundsWorkflowAdmissionBeforeStateAmplification) {
  FakeSkills skills;
  ManagerOptions options;
  options.max_workflows = 1;
  options.max_actions_per_workflow = 1;
  options.max_workflow_bytes = 512;
  Manager manager(skills.Runner(), {}, options);
  manager.Submit({{"workflow_id", "one"}, {"actions", {SkillAction("a")}}});
  EXPECT_THROW(manager.Submit({{"workflow_id", "two"}, {"actions", {SkillAction("a")}}}),
               std::length_error);
  EXPECT_THROW(manager.Submit({{"workflow_id", "many"},
                               {"actions", {SkillAction("a"), SkillAction("b")}}}),
               std::length_error);
  auto large = SkillAction("a");
  large["parameters"]["value"] = std::string(1024, 'x');
  EXPECT_THROW(manager.Submit({{"workflow_id", "one"}, {"actions", {large}}}),
               std::length_error);
}

TEST(ActionListManagerTest, EvictsOldestTerminalWorkflowAndCompactsJournal) {
  TemporaryDirectory directory;
  ManagerOptions options;
  options.journal_directory = directory.path;
  options.max_workflows = 2;
  {
    FakeSkills skills;
    Manager manager(skills.Runner(), {}, options);
    manager.Submit({{"workflow_id", "one"}, {"actions", {NoopAction("done")}}});
    manager.Submit({{"workflow_id", "two"}, {"actions", {NoopAction("done")}}});
    manager.Submit({{"workflow_id", "three"}, {"actions", {NoopAction("done")}}});
    EXPECT_FALSE(manager.Get("one"));
    EXPECT_TRUE(manager.Get("two"));
    EXPECT_TRUE(manager.Get("three"));
  }

  FakeSkills recovered_skills;
  Manager recovered(recovered_skills.Runner(), {}, options);
  EXPECT_FALSE(recovered.Get("one"));
  EXPECT_TRUE(recovered.Get("two"));
  EXPECT_TRUE(recovered.Get("three"));
}

TEST(ActionListManagerTest, NeverEvictsActiveWorkflowAtRetentionLimit) {
  FakeSkills skills;
  ManagerOptions options;
  options.max_workflows = 1;
  Manager manager(skills.Runner(), {}, options);
  manager.Submit({{"workflow_id", "active"}, {"actions", {SkillAction("hold")}}});
  EXPECT_THROW(manager.Submit({{"workflow_id", "other"}, {"actions", {NoopAction("done")}}}),
               std::length_error);
  EXPECT_TRUE(manager.Get("active"));
}

TEST(ActionListManagerTest, JournalFailurePutsManagerInExternallyOpaqueFailStop) {
  TemporaryDirectory directory;
  FakeSkills skills;
  ManagerOptions options;
  options.journal_directory = directory.path;
  options.journal_max_record_bytes = 4096;
  options.journal_max_bytes = 4300;
  Manager manager(skills.Runner(), {}, options);

  bool failed = false;
  for (std::uint64_t sequence = 1; sequence < 100 && !failed; ++sequence) {
    try {
      manager.UpdateFact("fact-" + std::to_string(sequence),
                         {std::string(512, 'x'), "fixture", sequence,
                          std::chrono::system_clock::now(), {}});
    } catch (const std::length_error&) {
      failed = true;
    }
  }
  ASSERT_TRUE(failed);
  EXPECT_THROW(manager.Snapshot(), std::runtime_error);
  EXPECT_THROW(manager.List(), std::runtime_error);
  EXPECT_THROW(manager.Get("anything"), std::runtime_error);
  EXPECT_THROW(manager.UpdateFact("later", {true, "fixture", 101, {}, {}}),
               std::runtime_error);
}

TEST(ActionListManagerTest, EvaluatesResourceConditionsOnExternalStateNotifications) {
  FakeSkills skills;
  std::optional<ResourceSnapshot> detection;
  ManagerOptions options;
  options.read_resource = [&](const std::string& uri) -> std::optional<ResourceSnapshot> {
    if (uri != "robots/demo-0/detection/state") return std::nullopt;
    return detection;
  };
  Manager manager(skills.Runner(), {}, std::move(options));

  EXPECT_THROW(
      manager.Submit({{"workflow_id", "mixed"},
                      {"actions",
                       {SkillAction("mixed", nlohmann::json::array(),
                                    nlohmann::json{{"resource", "robots/demo-0/detection/state"},
                                                   {"detection_kind", "human"},
                                                   {"detection_value_pointer", "/attributes/action"},
                                                   {"fact", "gate"},
                                                   {"equals", "waving"}})}}}),
      std::invalid_argument);
  EXPECT_THROW(
      manager.Submit({{"workflow_id", "bad-action"},
                      {"actions",
                       {SkillAction("bad", nlohmann::json::array(),
                                    nlohmann::json{{"resource", "robots/demo-0/detection/state"},
                                                   {"detection_kind", "person"},
                                                   {"detection_value_pointer", "/attributes/action"},
                                                   {"equals", "waving"}})}}}),
      std::invalid_argument);
  EXPECT_THROW(
      manager.Submit({{"workflow_id", "private-field"},
                      {"actions",
                       {SkillAction("bad", nlohmann::json::array(),
                                    nlohmann::json{{"resource", "robots/demo-0/detection/state"},
                                                   {"detection_kind", "human"},
                                                   {"detection_value_pointer", "/keypoints"},
                                                   {"equals", nlohmann::json::array()}})}}}),
      std::invalid_argument);

  manager.Submit({{"workflow_id", "resource"},
                  {"actions",
                   {SkillAction("assist", nlohmann::json::array(),
                                nlohmann::json{{"resource", "robots/demo-0/detection/state"},
                                               {"detection_kind", "human"},
                                               {"detection_value_pointer", "/attributes/pose"},
                                               {"equals", "sit"},
                                               {"max_age_ms", 5000}})}}});
  EXPECT_EQ(Action(*manager.Get("resource"), "assist").at("status"), "blocked");
  EXPECT_TRUE(skills.executions_.empty());

  detection = ResourceSnapshot{
      nlohmann::json{{"detections", nlohmann::json::array({nlohmann::json{
                                        {"kind", "human"},
                                        {"id", "human:1"},
                                        {"attributes", {{"pose", "stand"}, {"action", "none"}}},
                                    }})}},
      std::chrono::system_clock::now()};
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("resource"), "assist").at("status"), "blocked");
  EXPECT_TRUE(skills.executions_.empty());

  detection->value["detections"][0]["attributes"]["pose"] = "sit";
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("resource"), "assist").at("status"), "queued");
  ASSERT_EQ(skills.executions_.size(), 1u);

  detection->observed_at = std::chrono::system_clock::now() - std::chrono::seconds(6);
  detection->value["detections"][0]["attributes"]["pose"] = "stand";
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("resource"), "assist").at("status"), "queued");
}

TEST(ActionListManagerTest, GivesActionPriorityTimeToOverrideNoisyPosture) {
  FakeSkills skills;
  std::optional<ResourceSnapshot> detection;
  ManagerOptions options;
  options.read_resource = [&](const std::string&) { return detection; };
  Manager manager(skills.Runner(), {}, std::move(options));

  const std::string resource = "robots/demo-0/detection/state";
  auto fallen = SkillAction(
      "fallen", nlohmann::json::array(),
      nlohmann::json{{"resource", resource},
                     {"detection_kind", "human"},
                     {"detection_value_pointer", "/attributes/action"},
                     {"equals", "fallen"}});
  fallen["choice_group"] = "owner-interaction";
  auto furniture = SkillAction(
      "furniture", nlohmann::json::array(),
      nlohmann::json{{"resource", resource},
                     {"detection_kind", "human"},
                     {"detection_value_pointer", "/attributes/posture_state"},
                     {"equals", "lying_on_furniture"}});
  furniture["choice_group"] = "owner-interaction";
  manager.Submit({{"workflow_id", "priority"}, {"actions", {fallen, furniture}}});

  const auto first = std::chrono::system_clock::now();
  detection = ResourceSnapshot{
      nlohmann::json{{"detections", nlohmann::json::array({nlohmann::json{
                                        {"kind", "human"},
                                        {"id", "human:1"},
                                        {"attributes",
                                         {{"pose", "lie"},
                                          {"posture_state", "lying_on_furniture"},
                                          {"action", "none"}}},
                                    }})}},
      first};
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("priority"), "furniture").at("status"), "blocked");

  detection->observed_at = first + std::chrono::seconds(1);
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("priority"), "furniture").at("status"), "blocked");

  detection->observed_at = first + std::chrono::milliseconds(1600);
  detection->value["detections"][0]["attributes"]["action"] = "fallen";
  manager.NotifyExternalState();
  EXPECT_EQ(Action(*manager.Get("priority"), "fallen").at("status"), "queued");
  EXPECT_EQ(Action(*manager.Get("priority"), "furniture").at("status"), "skipped");
}

TEST(ActionListManagerTest, SelectsChoiceByMonitorFactAndStopsMonitor) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto monitor = SkillAction("observe");
  monitor["execution_mode"] = "monitor";
  monitor["monitor_lifetime"] = "explicit";
  auto waving = SkillAction(
      "waving", {"observe"},
      nlohmann::json{{"fact", "human"}, {"equals", "waving"}, {"source_action_id", "observe"}});
  waving["choice_group"] = "observation";
  auto none = SkillAction(
      "none", {"observe"},
      nlohmann::json{{"fact", "human"}, {"equals", "none"}, {"source_action_id", "observe"}});
  none.erase("skill");
  none["noop"] = nlohmann::json::object();
  none["choice_group"] = "observation";
  nlohmann::json stop{{"action_id", "stop"},
                      {"control", {{"target_action_id", "observe"}, {"operation", "terminate"}}},
                      {"parameters", nlohmann::json::object()},
                      {"depends_on", {"waving", "none"}}};

  manager.Submit({{"workflow_id", "choice"}, {"actions", {monitor, waving, none, stop}}});
  ASSERT_EQ(skills.executions_.size(), 1u);
  EXPECT_TRUE(manager.UpdateFact(
      "human", {"none", "other-execution", 1, std::chrono::system_clock::now(), {}}));
  EXPECT_EQ(Action(*manager.Get("choice"), "none").at("status"), "blocked");

  EXPECT_TRUE(manager.UpdateFact(
      "human",
      {"none", skills.executions_.front().execution_id, 1, std::chrono::system_clock::now(), {}}));
  auto selected = manager.Get("choice").value();
  EXPECT_EQ(Action(selected, "none").at("status"), "succeeded");
  EXPECT_EQ(Action(selected, "waving").at("status"), "skipped");
  EXPECT_EQ(Action(selected, "stop").at("status"), "succeeded");
  ASSERT_EQ(skills.cancelled_.size(), 1u);

  auto stopped = skills.executions_.front();
  stopped.status = skills::ExecutionStatus::kCancelled;
  manager.ObserveExecution(stopped);
  const auto complete = manager.Get("choice").value();
  EXPECT_EQ(Action(complete, "observe").at("status"), "stopped");
  EXPECT_EQ(complete.at("status"), "succeeded");
}

TEST(ActionListManagerTest, SkipsConditionalBranchesAfterToleratedObservationFailure) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto observe = SkillAction("observe");
  observe["continue_on_failure"] = true;
  auto waving = SkillAction(
      "waving", {"observe"},
      nlohmann::json{{"fact", "human"}, {"equals", "waving"},
                     {"source_action_id", "observe"}});
  waving["choice_group"] = "observation";
  auto none = SkillAction(
      "none", {"observe"},
      nlohmann::json{{"fact", "human"}, {"equals", "none"},
                     {"source_action_id", "observe"}});
  none["choice_group"] = "observation";

  manager.Submit({{"workflow_id", "failed-observation-choice"},
                  {"actions", {observe, waving, none,
                               SkillAction("cleanup", {"waving", "none"})}}});
  ASSERT_EQ(skills.executions_.size(), 1u);
  auto failed = skills.executions_.front();
  failed.status = skills::ExecutionStatus::kFailed;
  failed.reason_code = "observation.failed";
  manager.ObserveExecution(failed);

  ASSERT_EQ(skills.executions_.size(), 2u);
  auto cleanup = skills.executions_.back();
  cleanup.status = skills::ExecutionStatus::kSucceeded;
  cleanup.result = nlohmann::json::object();
  manager.ObserveExecution(cleanup);

  const auto complete = manager.Get("failed-observation-choice").value();
  EXPECT_EQ(Action(complete, "waving").at("status"), "skipped");
  EXPECT_EQ(Action(complete, "waving").at("reason"),
            "dependency_failed_tolerated:observe");
  EXPECT_EQ(Action(complete, "none").at("status"), "skipped");
  EXPECT_EQ(Action(complete, "none").at("reason"),
            "dependency_failed_tolerated:observe");
  EXPECT_EQ(Action(complete, "cleanup").at("status"), "succeeded");
  EXPECT_EQ(complete.at("status"), "succeeded");
}

TEST(ActionListManagerTest, StopsExplicitMonitorWhenWorkflowFails) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto monitor = SkillAction("monitor");
  monitor["execution_mode"] = "monitor";
  monitor["monitor_lifetime"] = "explicit";
  auto task = SkillAction("task", {"monitor"});

  manager.Submit({{"workflow_id", "failed-with-monitor"}, {"actions", {monitor, task}}});
  ASSERT_EQ(skills.executions_.size(), 2u);
  auto rejected = skills.executions_.back();
  rejected.status = skills::ExecutionStatus::kRejected;
  rejected.reason_code = "invalid.parameters";
  manager.ObserveExecution(rejected);

  ASSERT_EQ(skills.cancelled_.size(), 1u);
  EXPECT_EQ(skills.cancelled_.front(), skills.executions_.front().execution_id);
  EXPECT_EQ(manager.Get("failed-with-monitor")->at("status"), "failed");
}

TEST(ActionListManagerTest, StopsWorkflowLifetimeMonitorAfterTasksComplete) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto monitor = SkillAction("monitor");
  monitor["execution_mode"] = "monitor";
  monitor["monitor_lifetime"] = "workflow";
  auto finish = SkillAction("finish", {"monitor"});
  finish.erase("skill");
  finish["noop"] = nlohmann::json::object();

  manager.Submit({{"workflow_id", "workflow-monitor"}, {"actions", {monitor, finish}}});
  ASSERT_EQ(skills.executions_.size(), 1u);
  ASSERT_EQ(skills.cancelled_.size(), 1u);
  auto stopped = skills.executions_.front();
  stopped.status = skills::ExecutionStatus::kCancelled;
  manager.ObserveExecution(stopped);
  EXPECT_EQ(manager.Get("workflow-monitor")->at("status"), "succeeded");
}

TEST(ActionListManagerTest, StopsWorkflowLifetimeMonitorAfterToleratedFailure) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto monitor = SkillAction("monitor");
  monitor["execution_mode"] = "monitor";
  monitor["monitor_lifetime"] = "workflow";
  auto task = SkillAction("task", {"monitor"});
  task["continue_on_failure"] = true;

  manager.Submit({{"workflow_id", "tolerated-monitor"}, {"actions", {monitor, task}}});
  ASSERT_EQ(skills.executions_.size(), 2u);
  auto failed = skills.executions_.back();
  failed.status = skills::ExecutionStatus::kFailed;
  manager.ObserveExecution(failed);
  ASSERT_EQ(skills.cancelled_.size(), 1u);

  auto stopped = skills.executions_.front();
  stopped.status = skills::ExecutionStatus::kCancelled;
  manager.ObserveExecution(stopped);
  EXPECT_EQ(manager.Get("tolerated-monitor")->at("status"), "succeeded");
}

TEST(ActionListManagerTest, RejectsInvalidExtendedActionDefinitions) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  auto choice = SkillAction("choice");
  choice["choice_group"] = "group";
  EXPECT_THROW(manager.Submit({{"workflow_id", "choice-without-condition"}, {"actions", {choice}}}),
               std::invalid_argument);

  auto monitor = SkillAction("monitor");
  monitor["execution_mode"] = "monitor";
  EXPECT_THROW(
      manager.Submit({{"workflow_id", "monitor-without-lifetime"}, {"actions", {monitor}}}),
      std::invalid_argument);

  nlohmann::json control{{"action_id", "stop"},
                         {"control", {{"target_action_id", "missing"}, {"operation", "terminate"}}},
                         {"parameters", nlohmann::json::object()}};
  EXPECT_THROW(manager.Submit({{"workflow_id", "unknown-control-target"}, {"actions", {control}}}),
               std::invalid_argument);

  auto invalid_continue = SkillAction("invalid-continue");
  invalid_continue["continue_on_failure"] = "true";
  EXPECT_THROW(manager.Submit({{"workflow_id", "invalid-continue"},
                               {"actions", {invalid_continue}}}),
               std::invalid_argument);
}

TEST(ActionListManagerTest, MapsPrimitiveOutcomeOnceAndSupersedesRunningWork) {
  FakeSkills skills;
  std::atomic<int> calls{0};
  std::atomic<int> cancels{0};
  PrimitiveRunner primitives;
  primitives.execute = [&](const std::string& name, const std::string&, const std::string&,
                           nlohmann::json, const std::string&,
                           const std::function<bool()>& cancelled) {
    ++calls;
    if (name == "hold") {
      while (!cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return nlohmann::json{{"result", {{"status", "cancelled"}}}};
    }
    return nlohmann::json{{"result", {{"status", "succeeded"}}}};
  };
  primitives.cancel = [&](const std::string&) { ++cancels; };
  Manager manager(skills.Runner(), std::move(primitives), {2});
  manager.Submit({{"workflow_id", "old"}, {"actions", {PrimitiveAction("hold", "hold")}}});
  WaitFor(manager, "old",
          [](const auto& list) { return Action(list, "hold").value("status", "") == "running"; });
  manager.Submit({{"workflow_id", "new"},
                  {"supersedes_workflow_ids", {"old"}},
                  {"actions", {PrimitiveAction("replacement", "quick")}}});
  EXPECT_EQ(WaitFor(manager, "old",
                    [](const auto& list) { return list.value("status", "") == "superseded"; })
                .at("superseded_by_workflow_id"),
            "new");
  EXPECT_EQ(WaitFor(manager, "new",
                    [](const auto& list) { return list.value("status", "") == "succeeded"; })
                .at("status"),
            "succeeded");
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(cancels.load(), 1);
}

TEST(ActionListManagerTest, MapsEveryPrimitiveTerminalAndAmbiguousOutcomeWithoutRetry) {
  struct Case {
    const char* returned;
    const char* expected;
    bool throws;
  };
  const std::vector<Case> cases{
      {"succeeded", "succeeded", false},     {"rejected", "rejected", false},
      {"failed", "failed", false},           {"cancelled", "cancelled", false},
      {"timed_out", "timed_out", false},     {"outcome_unknown", "outcome_unknown", false},
      {"running", "outcome_unknown", false}, {"", "outcome_unknown", true}};
  for (std::size_t index = 0; index < cases.size(); ++index) {
    SCOPED_TRACE(index);
    FakeSkills skills;
    std::atomic<int> calls{0};
    PrimitiveRunner primitive;
    primitive.execute = [&, current = cases[index]](
                            const std::string&, const std::string&, const std::string&,
                            nlohmann::json, const std::string&, const std::function<bool()>&) {
      ++calls;
      if (current.throws) throw std::runtime_error("ambiguous");
      return nlohmann::json{{"result", {{"status", current.returned}}}};
    };
    Manager manager(skills.Runner(), std::move(primitive));
    const std::string id = "primitive-" + std::to_string(index);
    manager.Submit({{"workflow_id", id}, {"actions", {PrimitiveAction("execute")}}});
    const auto terminal = WaitFor(manager, id, [&](const auto& list) {
      return Action(list, "execute").value("status", "") == cases[index].expected;
    });
    EXPECT_EQ(terminal.at("status"),
              std::string(cases[index].expected) == "succeeded" ? "succeeded" : "failed");
    EXPECT_EQ(calls.load(), 1);
    EXPECT_TRUE(Action(terminal, "execute").contains("result"));
  }
}

TEST(ActionListManagerTest, ReplacesOnlyUntouchedSameIdAndSnapshotsAllState) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  const auto definition = [](const char* marker) {
    return nlohmann::json{{"workflow_id", "replaceable"},
                          {"actions",
                           {SkillAction("conditional", nlohmann::json::array(),
                                        nlohmann::json{{"fact", "gate"}, {"equals", marker}})}}};
  };
  const auto first = manager.Submit(definition("old"));
  const auto replacement = manager.Submit(definition("new"));
  EXPECT_EQ(replacement.at("revision"), first.at("revision").get<std::uint64_t>() + 1);
  EXPECT_EQ(replacement.at("created_at_unix_ms"), first.at("created_at_unix_ms"));
  EXPECT_TRUE(
      manager.UpdateFact("gate", {"new", "fixture", 1, std::chrono::system_clock::now(), {}}));
  EXPECT_THROW(manager.Submit(definition("third")), std::logic_error);
  const auto snapshot = manager.Snapshot();
  ASSERT_EQ(snapshot.action_lists.size(), 1u);
  ASSERT_EQ(snapshot.executions.size(), 1u);
  ASSERT_EQ(snapshot.facts.size(), 1u);
  EXPECT_EQ(snapshot.facts.at("gate").source, "fixture");
}

TEST(ActionListManagerTest, SnapshotCursorAtomicallyReplaysSubsequentMutations) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  const auto before = manager.Snapshot();
  ASSERT_FALSE(before.cursor.epoch.empty());
  EXPECT_EQ(before.cursor.sequence, 0u);
  manager.Submit({{"workflow_id", "watched"},
                  {"actions",
                   {SkillAction("blocked", nlohmann::json::array(),
                                nlohmann::json{{"fact", "gate"}, {"equals", true}})}}});
  auto replay = manager.WaitForEvents(before.cursor, std::chrono::milliseconds(0));
  ASSERT_EQ(replay.status, ReplayStatus::kOk);
  ASSERT_FALSE(replay.events.empty());
  EXPECT_EQ(replay.events.back().action_list.at("workflow_id"), "watched");
  const auto after = manager.Snapshot();
  EXPECT_EQ(after.cursor.sequence, replay.events.back().cursor.sequence);
  EXPECT_EQ(after.action_lists.size(), 1u);
}

TEST(ActionListManagerTest, ReportsEpochMismatchAndExpiredCursor) {
  FakeSkills skills;
  Manager manager(skills.Runner(), {}, {1, 2});
  const auto initial = manager.Snapshot().cursor;
  EXPECT_EQ(manager.WaitForEvents({"different-epoch", 0}, std::chrono::milliseconds(0)).status,
            ReplayStatus::kEpochMismatch);
  EXPECT_EQ(manager.WaitForEvents({initial.epoch, 1}, std::chrono::milliseconds(0)).status,
            ReplayStatus::kFuture);
  EXPECT_EQ(manager
                .WaitForEvents({initial.epoch, std::numeric_limits<std::uint64_t>::max()},
                               std::chrono::milliseconds(0))
                .status,
            ReplayStatus::kFuture);
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
    EXPECT_TRUE(manager.UpdateFact(
        "fact", {sequence, "fixture", sequence, std::chrono::system_clock::now(), {}}));
  EXPECT_EQ(manager.WaitForEvents(initial, std::chrono::milliseconds(0)).status,
            ReplayStatus::kExpired);
  const auto current = manager.Snapshot().cursor;
  EXPECT_EQ(manager.WaitForEvents(current, std::chrono::milliseconds(1)).status, ReplayStatus::kOk);
}

TEST(ActionListManagerTest, WakesLiveFollowerForAcceptedFact) {
  FakeSkills skills;
  Manager manager(skills.Runner());
  const auto cursor = manager.Snapshot().cursor;
  auto waiting = std::async(std::launch::async,
                            [&] { return manager.WaitForEvents(cursor, std::chrono::seconds(1)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  EXPECT_TRUE(
      manager.UpdateFact("live", {true, "fixture", 1, std::chrono::system_clock::now(), {}}));
  const auto batch = waiting.get();
  ASSERT_EQ(batch.status, ReplayStatus::kOk);
  ASSERT_EQ(batch.events.size(), 1u);
  EXPECT_EQ(batch.events.front().kind, Event::Kind::kFact);
  EXPECT_EQ(batch.events.front().fact_key, "live");
}

TEST(ActionListManagerTest, RecoversActionListsAndFactsAcrossRestart) {
  TemporaryDirectory directory;
  std::atomic<int> calls{0};
  {
    FakeSkills skills;
    PrimitiveRunner primitive;
    primitive.execute = [&](const std::string&, const std::string&, const std::string&,
                            nlohmann::json, const std::string&, const std::function<bool()>&) {
      ++calls;
      return nlohmann::json{{"result", {{"status", "succeeded"}}}};
    };
    Manager manager(skills.Runner(), std::move(primitive), {1, 4096, directory.path});
    EXPECT_TRUE(manager.UpdateFact(
        "gate", {true, "fixture", 1, std::chrono::system_clock::now(), {}}));
    manager.Submit({{"workflow_id", "durable"}, {"actions", {PrimitiveAction("move")}}});
    WaitFor(manager, "durable",
            [](const auto& list) { return list.value("status", "") == "succeeded"; });
  }

  FakeSkills recovered_skills;
  PrimitiveRunner recovered_primitive;
  recovered_primitive.execute = [&](const std::string&, const std::string&, const std::string&,
                                    nlohmann::json, const std::string&,
                                    const std::function<bool()>&) {
    ++calls;
    return nlohmann::json{{"result", {{"status", "succeeded"}}}};
  };
  Manager recovered(recovered_skills.Runner(), std::move(recovered_primitive),
                    {1, 4096, directory.path});
  ASSERT_TRUE(recovered.Get("durable"));
  EXPECT_EQ(recovered.Get("durable")->at("status"), "succeeded");
  ASSERT_EQ(recovered.Snapshot().facts.size(), 1u);
  EXPECT_EQ(recovered.Snapshot().facts.at("gate").value, true);
  EXPECT_EQ(calls.load(), 1);
}

TEST(ActionListManagerTest, RecoversBindingsResolvedParametersAndOutputsAcrossRestart) {
  TemporaryDirectory directory;
  nlohmann::json dispatched;
  {
    FakeSkills skills;
    PrimitiveRunner primitive;
    primitive.execute = [&](const std::string& name, const std::string&, const std::string&,
                            nlohmann::json arguments, const std::string&,
                            const std::function<bool()>&) {
      if (name == "produce")
        return nlohmann::json{
            {"result", {{"status", "succeeded"}, {"value", {{"id", 17}}}}}};
      dispatched = std::move(arguments);
      return nlohmann::json{{"result", {{"status", "succeeded"}}}};
    };
    Manager manager(skills.Runner(), std::move(primitive), {1, 4096, directory.path});
    auto consume = PrimitiveAction("consume", "consume");
    consume["parameters"] = {{"selected", {{"id", 0}}}};
    consume["output_bindings"] = {Binding("produce", "/value", "/selected")};
    manager.Submit({{"workflow_id", "durable-bindings"},
                    {"actions", {PrimitiveAction("produce", "produce"), consume}}});
    WaitFor(manager, "durable-bindings",
            [](const auto& list) { return list.at("status") == "succeeded"; });
  }
  EXPECT_EQ(dispatched, (nlohmann::json{{"selected", {{"id", 17}}}}));

  FakeSkills recovered_skills;
  PrimitiveRunner recovered_primitive;
  recovered_primitive.execute = [](const std::string&, const std::string&, const std::string&,
                                   nlohmann::json, const std::string&,
                                   const std::function<bool()>&) {
    ADD_FAILURE() << "completed journal actions must not execute during replay";
    return nlohmann::json::object();
  };
  Manager recovered(recovered_skills.Runner(), std::move(recovered_primitive),
                    {1, 4096, directory.path});
  const auto list = recovered.Get("durable-bindings").value();
  EXPECT_EQ(list.at("status"), "succeeded");
  EXPECT_EQ(Action(list, "produce").at("output").at("value").at("id"), 17);
  const auto consumed = Action(list, "consume");
  EXPECT_EQ(consumed.at("parameters").at("selected").at("id"), 0);
  EXPECT_EQ(consumed.at("resolved_parameters").at("selected").at("id"), 17);
  EXPECT_EQ(consumed.at("output_bindings").front().at("source_action_id"), "produce");
}

TEST(ActionListManagerTest, MakesRecoveredInFlightActionUnknownWithoutExecutingIt) {
  TemporaryDirectory directory;
  Journal journal(directory.path);
  journal.Append(
      "action_list",
      {{"workflow_id", "interrupted"},
       {"revision", 2},
       {"status", "running"},
       {"created_at_unix_ms", 1},
       {"updated_at_unix_ms", 2},
       {"supersedes_workflow_ids", nlohmann::json::array()},
       {"actions",
        {{{"action_id", "move"},
          {"primitive", {{"name", "test.primitive"}, {"version", "1.0.0"}, {"device_id", "d"}}},
          {"parameters", nlohmann::json::object()},
          {"depends_on", nlohmann::json::array()},
          {"command_id", "possibly-executed"},
          {"status", "queued"}}}}});

  std::atomic<int> calls{0};
  FakeSkills skills;
  PrimitiveRunner primitive;
  primitive.execute = [&](const std::string&, const std::string&, const std::string&,
                          nlohmann::json, const std::string&, const std::function<bool()>&) {
    ++calls;
    return nlohmann::json::object();
  };
  {
    Manager recovered(skills.Runner(), primitive, {1, 4096, directory.path});
    const auto list = recovered.Get("interrupted").value();
    EXPECT_EQ(list.at("status"), "failed");
    EXPECT_EQ(Action(list, "move").at("status"), "outcome_unknown");
    EXPECT_EQ(calls.load(), 0);
  }
  Manager recovered_again(skills.Runner(), primitive, {1, 4096, directory.path});
  EXPECT_EQ(Action(*recovered_again.Get("interrupted"), "move").at("status"),
            "outcome_unknown");
  EXPECT_EQ(calls.load(), 0);
}

}  // namespace
}  // namespace eos::runtime::actionlist

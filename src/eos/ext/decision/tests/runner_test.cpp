#include "src/eos/ext/decision/runner.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace eos::ext::decision {
namespace {

class FakeTransport final : public DecisionTransport {
 public:
  Result<std::vector<SkillDescriptor>> ListSkills() override {
    return Result<std::vector<SkillDescriptor>>::Success({});
  }
  Result<SkillDescriptor> GetSkill(const SkillReference&) override {
    return Result<SkillDescriptor>::Failure(12, "unused");
  }
  Result<ExecutionView> StartExecution(const StartAction& action) override {
    starts.push_back(action);
    ExecutionView result;
    result.execution_id = "started";
    return Result<ExecutionView>::Success(std::move(result));
  }
  Result<std::vector<ExecutionView>> ListExecutions() override {
    return Result<std::vector<ExecutionView>>::Success({});
  }
  Result<ExecutionView> GetExecution(const std::string&) override {
    return Result<ExecutionView>::Failure(12, "unused");
  }
  Result<SignalReceipt> SignalExecution(const SignalAction& action) override {
    signals.push_back(action);
    return Result<SignalReceipt>::Success({"signal", action.execution_id, action.request_id, true});
  }
  Result<ExecutionView> TerminateExecution(const std::string& execution_id) override {
    cancellations.push_back(execution_id);
    ExecutionView result;
    result.execution_id = execution_id;
    return Result<ExecutionView>::Success(std::move(result));
  }
  Result<TerminateExecutionsReceipt> TerminateExecutions(
      const TerminateExecutionsAction&) override {
    return Result<TerminateExecutionsReceipt>::Failure(12, "unused");
  }
  TransportError FollowExecutionJournal(const std::string&, std::uint64_t after_sequence,
                                        const JournalHandler& handler) override {
    cursors.push_back(after_sequence);
    ++watch_calls;
    DecisionJournalEntry event;
    event.journal_sequence = watch_calls == 1 ? 4 : 5;
    if (record) {
      JournalRecordView value;
      value.execution_id = "observed";
      value.name = "work.started";
      event.value = std::move(value);
    } else {
      ExecutionView execution;
      execution.execution_id = "observed";
      event.value = std::move(execution);
    }
    if (!handler(event)) return {1, "callback stopped"};
    return watch_calls == 1 ? TransportError{14, "reconnect"} : TransportError{};
  }
  TransportError MonitorExecutions(const ExecutionSelector&, std::uint64_t,
                                   const MonitorHandler&) override {
    return {12, "unused"};
  }
  void CancelPendingCalls() override { cancelled = true; }

  int watch_calls = 0;
  bool cancelled = false;
  bool record = false;
  std::vector<std::uint64_t> cursors;
  std::vector<StartAction> starts;
  std::vector<SignalAction> signals;
  std::vector<std::string> cancellations;
};

class RecordingPolicy final : public DecisionPolicy {
 public:
  std::vector<DecisionAction> Decide(const DecisionJournalEntry& event) override {
    seen.push_back(event.journal_sequence);
    if (event.journal_sequence == 4) {
      SignalAction action;
      action.execution_id = "observed";
      action.request_id = "stable-signal-request";
      action.name = "navigation.updated";
      return {action};
    }
    StartAction action;
    action.reference = {"follow.up", "1.0.0"};
    action.request_id = "stable-start-request";
    return {action};
  }

  std::vector<std::uint64_t> seen;
};

TEST(DecisionRunnerTest, ReconnectsFromCommittedCursorAndPreservesRequestIds) {
  FakeTransport transport;
  RecordingPolicy policy;
  DecisionRunner runner(transport, policy, RunnerOptions{std::chrono::milliseconds(0)});

  const auto result = runner.Run("observed");

  EXPECT_FALSE(result);
  EXPECT_EQ(transport.cursors, (std::vector<std::uint64_t>{0, 4}));
  EXPECT_EQ(policy.seen, (std::vector<std::uint64_t>{4, 5}));
  ASSERT_EQ(transport.signals.size(), 1U);
  EXPECT_EQ(transport.signals.front().request_id, "stable-signal-request");
  ASSERT_EQ(transport.starts.size(), 1U);
  EXPECT_EQ(transport.starts.front().request_id, "stable-start-request");
  EXPECT_EQ(runner.Cursor("observed"), 5U);
}

TEST(DecisionRunnerTest, StopCancelsPendingTransportCalls) {
  FakeTransport transport;
  RecordingPolicy policy;
  DecisionRunner runner(transport, policy);

  runner.Stop();

  EXPECT_TRUE(transport.cancelled);
}

TEST(DecisionRunnerTest, IgnoresJournalRecordsByDefaultWhileAdvancingCursor) {
  FakeTransport transport;
  transport.record = true;
  RecordingPolicy policy;
  DecisionRunner runner(transport, policy, RunnerOptions{std::chrono::milliseconds(0)});

  EXPECT_FALSE(runner.Run("observed"));
  EXPECT_TRUE(policy.seen.empty());
  EXPECT_EQ(runner.Cursor("observed"), 5U);
}

}  // namespace
}  // namespace eos::ext::decision

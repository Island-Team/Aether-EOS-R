#include "runner.h"

#include <algorithm>
#include <thread>
#include <type_traits>

namespace eos::ext::decision {
namespace {

bool Retryable(int code) {
  // gRPC CANCELLED, UNKNOWN, DEADLINE_EXCEEDED, RESOURCE_EXHAUSTED, ABORTED,
  // INTERNAL, UNAVAILABLE, and DATA_LOSS may be transient at this boundary.
  return code == 1 || code == 2 || code == 4 || code == 8 || code == 10 || code == 13 ||
         code == 14 || code == 15;
}

}  // namespace

DecisionRunner::DecisionRunner(DecisionTransport& transport, DecisionPolicy& policy,
                               RunnerOptions options)
    : transport_(transport), policy_(policy), options_(options) {}

DecisionRunner::~DecisionRunner() { Stop(); }

TransportError DecisionRunner::Run(const std::string& execution_id) {
  stopped_.store(false);
  while (!stopped_.load()) {
    TransportError callback_error;
    const auto status = transport_.FollowExecutionJournal(
        execution_id, Cursor(execution_id),
        [this, &execution_id, &callback_error](const DecisionJournalEntry& entry) {
          return OnEntry(execution_id, entry, &callback_error);
        });
    if (callback_error) return callback_error;
    if (stopped_.load()) return {};
    if (!status) return {};
    if (!Retryable(status.code)) return status;
    std::this_thread::sleep_for(options_.reconnect_delay);
  }
  return {};
}

void DecisionRunner::Stop() {
  stopped_.store(true);
  transport_.CancelPendingCalls();
}

std::uint64_t DecisionRunner::Cursor(const std::string& execution_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cursors_.find(execution_id);
  return found == cursors_.end() ? 0 : found->second;
}

bool DecisionRunner::OnEntry(const std::string& execution_id, const DecisionJournalEntry& entry,
                             TransportError* error) {
  if (stopped_.load()) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& cursor = cursors_[execution_id];
    if (entry.journal_sequence <= cursor) return true;
  }

  const bool ignored =
      std::holds_alternative<std::monostate>(entry.value) ||
      (std::holds_alternative<JournalRecordView>(entry.value) && !options_.deliver_journal_records);
  if (ignored) {
    std::lock_guard<std::mutex> lock(mutex_);
    cursors_[execution_id] = entry.journal_sequence;
    return !stopped_.load();
  }

  for (const auto& action : policy_.Decide(entry)) {
    *error = Submit(action);
    if (*error) return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    cursors_[execution_id] = entry.journal_sequence;
  }
  return !stopped_.load();
}

TransportError DecisionRunner::Submit(const DecisionAction& action) {
  return std::visit(
      [this](const auto& value) -> TransportError {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, StartAction>) {
          const auto result = transport_.StartExecution(value);
          return result.ok() ? TransportError{} : result.error;
        } else if constexpr (std::is_same_v<T, SignalAction>) {
          const auto result = transport_.SignalExecution(value);
          return result.ok() ? TransportError{} : result.error;
        } else {
          const auto result = transport_.TerminateExecution(value.execution_id);
          return result.ok() ? TransportError{} : result.error;
        }
      },
      action);
}

}  // namespace eos::ext::decision

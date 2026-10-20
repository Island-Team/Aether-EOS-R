#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "src/eos/runtime/skills/core/model.h"
namespace eos::runtime::skills::detail {
struct SignalEnvelope {
  Signal signal;
  std::string request_id;
  SignalReceipt receipt;
};
struct Record {
  mutable std::mutex mutex;
  std::condition_variable signal_available;
  ExecutionSnapshot snapshot;
  std::vector<ExecutionSnapshot> lifecycle;
  std::deque<JournalEntry> events;
  std::deque<SignalEnvelope> inbound;
  std::unordered_map<std::string, SignalEnvelope> signal_requests;
  std::deque<IpcMessage> mailbox;
  std::deque<IpcMessage> ipc_requests;
  std::unordered_set<std::string> children;
  std::optional<RetainedExecutionState> execution_state;
  std::optional<RetainedExecutionState> related_process_state;
  std::size_t process_depth{0};
  std::uint64_t process_generation{0};
  std::optional<ProcessHandle> parent_process;
  nlohmann::json arguments;
  std::optional<std::chrono::milliseconds> timeout;
  bool cancel_requested{false};
  bool cancel_delivered{false};
  bool provider_completed{false};
  std::optional<eos::lib::MonotonicTime> deadline;
  eos::lib::MonotonicTime created_monotonic{};
  std::optional<eos::lib::MonotonicTime> started_monotonic;
  std::optional<eos::lib::MonotonicTime> finished_monotonic;
  std::unordered_map<std::string, eos::lib::MonotonicTime> activity_started;
  std::unordered_map<std::string, ExecutionSnapshot::ActivityView> active_activities;
  std::vector<std::string> activity_order;
  std::size_t checkpoints_accepted{0};
  std::size_t automatic_activity_events{0};
  bool suppression_logged{false};
};
class RecordStore {
 public:
  std::unordered_map<std::string, std::shared_ptr<Record>> records;
  std::unordered_map<std::string, std::string> requests;
  std::deque<std::string> order;
  std::vector<std::string> EvictForCapacity(std::size_t limit);
};
}  // namespace eos::runtime::skills::detail

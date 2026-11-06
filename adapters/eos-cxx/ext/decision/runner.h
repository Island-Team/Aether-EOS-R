/// Event-loop glue between SkillService and a product-owned decision policy.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "client.h"
#include "policy.h"

namespace eos::ext::decision {

struct RunnerOptions {
  std::chrono::milliseconds reconnect_delay{250};
  bool deliver_journal_records{false};
};

class DecisionRunner final {
 public:
  DecisionRunner(DecisionTransport& transport, DecisionPolicy& policy, RunnerOptions options = {});
  ~DecisionRunner();
  DecisionRunner(const DecisionRunner&) = delete;
  DecisionRunner& operator=(const DecisionRunner&) = delete;

  /// Blocks while following one execution. Retryable stream failures reconnect
  /// from the last successfully processed journal cursor.
  TransportError Run(const std::string& execution_id);
  void Stop();

  std::uint64_t Cursor(const std::string& execution_id) const;

 private:
  bool OnEntry(const std::string& execution_id, const DecisionJournalEntry& entry,
               TransportError* error);
  TransportError Submit(const DecisionAction& action);

  DecisionTransport& transport_;
  DecisionPolicy& policy_;
  RunnerOptions options_;
  std::atomic<bool> stopped_{false};
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> cursors_;
};

}  // namespace eos::ext::decision

/// Policy seam for product-owned decision logic.
#pragma once

#include <vector>

#include "types.h"

namespace eos::ext::decision {

class DecisionPolicy {
 public:
  virtual ~DecisionPolicy() = default;

  /// Called once for each journal entry, in journal-sequence order. Returned
  /// actions are submitted in vector order before the next entry is delivered.
  virtual std::vector<DecisionAction> Decide(const DecisionJournalEntry& entry) = 0;
};

}  // namespace eos::ext::decision

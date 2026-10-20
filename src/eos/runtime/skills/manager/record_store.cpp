#include "src/eos/runtime/skills/manager/record_store.h"
namespace eos::runtime::skills::detail {
std::vector<std::string> RecordStore::EvictForCapacity(std::size_t limit) {
  std::vector<std::string> evicted;
  for (auto candidate = order.begin(); records.size() >= limit && candidate != order.end();) {
    auto found = records.find(*candidate);
    if (found == records.end()) {
      candidate = order.erase(candidate);
      continue;
    }
    if (found->second.use_count() != 1) {
      ++candidate;
      continue;
    }
    std::string request_id;
    std::string execution_id;
    {
      std::lock_guard<std::mutex> lock(found->second->mutex);
      if (!IsTerminal(found->second->snapshot.status)) {
        ++candidate;
        continue;
      }
      request_id = found->second->snapshot.request_id;
      execution_id = found->second->snapshot.execution_id;
    }
    if (request_id.empty()) {
      ++candidate;
      continue;
    }
    requests.erase(request_id);
    evicted.push_back(std::move(execution_id));
    records.erase(found);
    candidate = order.erase(candidate);
  }
  return evicted;
}
}  // namespace eos::runtime::skills::detail

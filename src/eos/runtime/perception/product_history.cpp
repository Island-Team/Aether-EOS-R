#include "src/eos/runtime/perception/product_history.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace eos::runtime::perception {
namespace detail {

struct ProductHistoryState {
  ProductHistoryState(ProductDeclaration declared, std::size_t retained)
      : declaration(std::move(declared)), capacity(retained) {}

  ProductDeclaration declaration;
  const std::size_t capacity;
  mutable std::mutex mutex;
  mutable std::condition_variable changed;
  std::deque<ProductPtr> products;
  std::uint64_t next_sequence{1};
  std::uint64_t evicted_through{0};
  bool writer_issued{false};
  bool closed{false};
};

}  // namespace detail
namespace {

using State = detail::ProductHistoryState;
constexpr auto kCancellationPoll = std::chrono::milliseconds(10);

RetentionState RetentionLocked(const State& state) {
  return {
      state.products.empty() ? 0 : state.products.front()->sequence,
      state.products.empty() ? state.evicted_through : state.products.back()->sequence,
      state.evicted_through,
      state.closed,
  };
}

ProductLookup LookupLocked(const State& state, ProductPtr product, bool gap = false) {
  return {std::move(product), RetentionLocked(state), gap};
}

bool TimePrefixWasEvicted(const State& state, std::chrono::system_clock::time_point time,
                          bool include_equal) {
  if (state.evicted_through == 0) return false;
  if (state.products.empty()) return true;
  return include_equal ? time <= state.products.front()->observed_at
                       : time < state.products.front()->observed_at;
}

void ValidateLimit(std::size_t limit) {
  if (limit == 0) throw std::invalid_argument("product history range limit must be non-zero");
}

}  // namespace

ProductHistoryReader::ProductHistoryReader(std::shared_ptr<State> state) : state_(std::move(state)) {}

const ProductDeclaration& ProductHistoryReader::declaration() const noexcept {
  return state_->declaration;
}

ProductLookup ProductHistoryReader::Latest() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return LookupLocked(*state_, state_->products.empty() ? nullptr : state_->products.back());
}

ProductLookup ProductHistoryReader::AtSequence(std::uint64_t sequence) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductPtr product;
  if (!state_->products.empty() && sequence >= state_->products.front()->sequence &&
      sequence <= state_->products.back()->sequence) {
    product = state_->products[static_cast<std::size_t>(sequence -
                                                        state_->products.front()->sequence)];
  }
  return LookupLocked(*state_, std::move(product),
                      sequence != 0 && sequence <= state_->evicted_through);
}

ProductLookup ProductHistoryReader::AtTime(std::chrono::system_clock::time_point time) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductPtr product;
  for (const auto& candidate : state_->products) {
    if (candidate->observed_at == time) product = candidate;
    if (candidate->observed_at > time) break;
  }
  return LookupLocked(*state_, std::move(product), TimePrefixWasEvicted(*state_, time, false));
}

ProductLookup ProductHistoryReader::Before(std::chrono::system_clock::time_point time) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductPtr product;
  for (const auto& candidate : state_->products) {
    if (candidate->observed_at >= time) break;
    product = candidate;
  }
  return LookupLocked(*state_, std::move(product), TimePrefixWasEvicted(*state_, time, true));
}

ProductLookup ProductHistoryReader::After(std::chrono::system_clock::time_point time) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductPtr product;
  for (const auto& candidate : state_->products) {
    if (candidate->observed_at > time) {
      product = candidate;
      break;
    }
  }
  return LookupLocked(*state_, std::move(product), TimePrefixWasEvicted(*state_, time, false));
}

ProductLookup ProductHistoryReader::Nearest(std::chrono::system_clock::time_point time) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductPtr before;
  ProductPtr after;
  for (const auto& candidate : state_->products) {
    if (candidate->observed_at <= time) {
      before = candidate;
    } else {
      after = candidate;
      break;
    }
  }
  ProductPtr product;
  if (!before) {
    product = after;
  } else if (!after || time - before->observed_at <= after->observed_at - time) {
    product = before;
  } else {
    product = after;
  }
  return LookupLocked(*state_, std::move(product), TimePrefixWasEvicted(*state_, time, false));
}

ProductRange ProductHistoryReader::RangeBySequence(std::uint64_t first, std::uint64_t last,
                                                    std::size_t limit) const {
  ValidateLimit(limit);
  if (first == 0) throw std::invalid_argument("product history sequence range must start above zero");
  if (first > last) throw std::invalid_argument("product history sequence range is reversed");
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductRange result;
  result.retention = RetentionLocked(*state_);
  result.retention_gap = first != 0 && first <= state_->evicted_through;
  for (const auto& product : state_->products) {
    if (product->sequence < first) continue;
    if (product->sequence > last) break;
    if (result.products.size() == limit) {
      result.truncated = true;
      break;
    }
    result.products.push_back(product);
  }
  return result;
}

ProductRange ProductHistoryReader::RangeByTime(std::chrono::system_clock::time_point first,
                                                std::chrono::system_clock::time_point last,
                                                std::size_t limit) const {
  ValidateLimit(limit);
  if (first > last) throw std::invalid_argument("product history time range is reversed");
  std::lock_guard<std::mutex> lock(state_->mutex);
  ProductRange result;
  result.retention = RetentionLocked(*state_);
  result.retention_gap = TimePrefixWasEvicted(*state_, first, true);
  for (const auto& product : state_->products) {
    if (product->observed_at < first) continue;
    if (product->observed_at > last) break;
    if (result.products.size() == limit) {
      result.truncated = true;
      break;
    }
    result.products.push_back(product);
  }
  return result;
}

ProductWait ProductHistoryReader::WaitNewer(
    std::uint64_t after_sequence, const std::function<bool()>& cancel_requested) const {
  std::unique_lock<std::mutex> lock(state_->mutex);
  for (;;) {
    const auto found = std::find_if(state_->products.begin(), state_->products.end(),
                                    [after_sequence](const ProductPtr& product) {
                                      return product->sequence > after_sequence;
                                    });
    if (found != state_->products.end()) {
      return {WaitStatus::kProduct, *found, RetentionLocked(*state_),
              after_sequence < state_->evicted_through};
    }
    if (cancel_requested) {
      const auto next_sequence = state_->next_sequence;
      lock.unlock();
      const bool cancelled = cancel_requested();
      lock.lock();
      if (cancelled) {
        return {WaitStatus::kCancelled, nullptr, RetentionLocked(*state_),
                after_sequence < state_->evicted_through};
      }
      if (state_->next_sequence != next_sequence) continue;
    }
    if (state_->closed) {
      return {WaitStatus::kClosed, nullptr, RetentionLocked(*state_),
              after_sequence < state_->evicted_through};
    }
    if (cancel_requested) {
      state_->changed.wait_for(lock, kCancellationPoll);
    } else {
      state_->changed.wait(lock);
    }
  }
}

ProductHistoryWriter::ProductHistoryWriter(std::shared_ptr<State> state) : state_(std::move(state)) {}

ProductPtr ProductHistoryWriter::Append(std::chrono::system_clock::time_point observed_at,
                                        nlohmann::json value) {
  if (!state_) throw std::logic_error("product history writer was moved from");
  if (!value.is_object()) throw std::invalid_argument("product history value must be an object");
  state_->declaration.validate(value);
  ProductPtr product;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->closed) throw std::logic_error("product history is closed");
    if (!state_->products.empty() && observed_at < state_->products.back()->observed_at)
      throw std::invalid_argument("product history time must be monotonic");
    if (state_->next_sequence == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("product history sequence exhausted");
    product = std::make_shared<const Product>(Product{state_->next_sequence, observed_at,
                                                      state_->declaration.boot_id,
                                                      state_->declaration.epoch,
                                                      std::move(value)});
    state_->products.push_back(product);
    ++state_->next_sequence;
    if (state_->products.size() > state_->capacity) {
      state_->evicted_through = state_->products.front()->sequence;
      state_->products.pop_front();
    }
  }
  state_->changed.notify_all();
  return product;
}

ProductHistory::ProductHistory(ProductDeclaration declaration, std::size_t capacity) {
  if (declaration.uri.empty() || declaration.schema_uri.empty() || declaration.boot_id.empty() ||
      declaration.epoch.empty())
    throw std::invalid_argument("product URI, schema URI, boot ID, and epoch are required");
  if (!declaration.validate)
    throw std::invalid_argument("product history schema validator is required");
  if (capacity == 0) throw std::invalid_argument("product history capacity must be non-zero");
  state_ = std::make_shared<State>(std::move(declaration), capacity);
}

ProductHistory::~ProductHistory() { Close(); }

ProductHistoryReader ProductHistory::Reader() const { return ProductHistoryReader(state_); }

ProductHistoryWriter ProductHistory::TakeWriter() {
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->writer_issued) throw std::logic_error("product history writer already issued");
  state_->writer_issued = true;
  return ProductHistoryWriter(state_);
}

void ProductHistory::Close() noexcept {
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
  }
  state_->changed.notify_all();
}

}  // namespace eos::runtime::perception

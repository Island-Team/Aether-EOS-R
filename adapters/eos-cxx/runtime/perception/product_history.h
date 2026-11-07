#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace eos::runtime::perception {

struct ProductDeclaration {
  using Validator = std::function<void(const nlohmann::json&)>;

  std::string uri;
  std::string schema_uri;
  std::string boot_id;
  std::string epoch;
  Validator validate;
};

struct Product final {
  std::uint64_t sequence;
  std::chrono::system_clock::time_point observed_at;
  std::string boot_id;
  std::string epoch;
  nlohmann::json value;
};

using ProductPtr = std::shared_ptr<const Product>;

struct RetentionState {
  std::uint64_t first_available_sequence{0};
  std::uint64_t latest_sequence{0};
  std::uint64_t evicted_through_sequence{0};
  bool closed{false};
};

struct ProductLookup {
  ProductPtr product;
  RetentionState retention;
  bool retention_gap{false};
};

struct ProductRange {
  std::vector<ProductPtr> products;
  RetentionState retention;
  bool retention_gap{false};
  bool truncated{false};
};

enum class WaitStatus {
  kProduct,
  kCancelled,
  kClosed,
};

struct ProductWait {
  WaitStatus status{WaitStatus::kClosed};
  ProductPtr product;
  RetentionState retention;
  bool retention_gap{false};
};

namespace detail {
struct ProductHistoryState;
}

/// Read-only capability for one declared perception product stream.
class ProductHistoryReader final {
 public:
  const ProductDeclaration& declaration() const noexcept;

  ProductLookup Latest() const;
  ProductLookup AtSequence(std::uint64_t sequence) const;
  /// Returns the highest-sequence product at exactly `time`.
  ProductLookup AtTime(std::chrono::system_clock::time_point time) const;
  ProductLookup Before(std::chrono::system_clock::time_point time) const;
  ProductLookup After(std::chrono::system_clock::time_point time) const;
  ProductLookup Nearest(std::chrono::system_clock::time_point time) const;

  /// Returns at most `limit` products in ascending sequence order. Bounds are inclusive.
  ProductRange RangeBySequence(std::uint64_t first, std::uint64_t last,
                               std::size_t limit) const;
  /// Returns at most `limit` products in ascending time/sequence order. Bounds are inclusive.
  ProductRange RangeByTime(std::chrono::system_clock::time_point first,
                           std::chrono::system_clock::time_point last,
                           std::size_t limit) const;

  /// Waits for the first retained product newer than `after_sequence`. The
  /// cancellation probe is invoked without holding the history lock.
  ProductWait WaitNewer(std::uint64_t after_sequence,
                        const std::function<bool()>& cancel_requested = {}) const;

 private:
  friend class ProductHistory;
  explicit ProductHistoryReader(std::shared_ptr<detail::ProductHistoryState> state);
  std::shared_ptr<detail::ProductHistoryState> state_;
};

/// Move-only runtime publishing capability. It is intentionally absent from reader handles.
class ProductHistoryWriter final {
 public:
  ProductHistoryWriter(ProductHistoryWriter&&) noexcept = default;
  ProductHistoryWriter& operator=(ProductHistoryWriter&&) noexcept = default;
  ProductHistoryWriter(const ProductHistoryWriter&) = delete;
  ProductHistoryWriter& operator=(const ProductHistoryWriter&) = delete;

  /// Appends one object accepted by the stream's declared schema validator.
  ProductPtr Append(std::chrono::system_clock::time_point observed_at, nlohmann::json value);

 private:
  friend class ProductHistory;
  explicit ProductHistoryWriter(std::shared_ptr<detail::ProductHistoryState> state);
  std::shared_ptr<detail::ProductHistoryState> state_;
};

/// Owns one bounded history and its lifecycle. Runtime code passes only Reader() northward.
class ProductHistory final {
 public:
  ProductHistory(ProductDeclaration declaration, std::size_t capacity);
  ~ProductHistory();
  ProductHistory(const ProductHistory&) = delete;
  ProductHistory& operator=(const ProductHistory&) = delete;
  ProductHistory(ProductHistory&&) = delete;
  ProductHistory& operator=(ProductHistory&&) = delete;

  ProductHistoryReader Reader() const;
  /// Issues the stream's sole writer capability. A second call throws std::logic_error.
  ProductHistoryWriter TakeWriter();
  void Close() noexcept;

 private:
  std::shared_ptr<detail::ProductHistoryState> state_;
};

}  // namespace eos::runtime::perception

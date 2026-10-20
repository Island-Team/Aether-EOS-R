#include "src/eos/runtime/perception/product_history.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <type_traits>

namespace eos::runtime::perception {
namespace {

using namespace std::chrono_literals;
using Time = std::chrono::system_clock::time_point;

ProductDeclaration Declaration() {
  return {"island://perception/detections", "island://schemas/detections.v1.json", "boot-7",
          "epoch-3", [](const nlohmann::json& value) {
            if (!value.contains("label") && !value.contains("value") &&
                !value.contains("sequence")) {
              throw std::invalid_argument("value does not match the test schema");
            }
          }};
}

TEST(ProductHistoryTest, DeclaresStreamAndSeparatesReadAndWriteCapabilities) {
  static_assert(!std::is_copy_constructible_v<ProductHistoryWriter>);
  static_assert(std::is_copy_constructible_v<ProductHistoryReader>);

  ProductHistory history(Declaration(), 3);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  EXPECT_EQ(reader.declaration().uri, "island://perception/detections");
  EXPECT_EQ(reader.declaration().schema_uri, "island://schemas/detections.v1.json");
  EXPECT_THROW(history.TakeWriter(), std::logic_error);

  const auto product = writer.Append(Time{} + 1s, {{"label", "cup"}});
  EXPECT_EQ(product->sequence, 1u);
  EXPECT_EQ(product->boot_id, "boot-7");
  EXPECT_EQ(product->epoch, "epoch-3");
  EXPECT_EQ(reader.Latest().product, product);
}

TEST(ProductHistoryTest, EnforcesMonotonicTimeAndSequence) {
  ProductHistory history(Declaration(), 3);
  auto writer = history.TakeWriter();
  EXPECT_EQ(writer.Append(Time{} + 1s, {{"value", 1}})->sequence, 1u);
  EXPECT_EQ(writer.Append(Time{} + 1s, {{"value", 2}})->sequence, 2u);
  EXPECT_THROW(writer.Append(Time{} + 999ms, {{"value", 3}}), std::invalid_argument);
  EXPECT_EQ(writer.Append(Time{} + 2s, {{"value", 4}})->sequence, 3u);
}

TEST(ProductHistoryTest, RejectsNonObjectsAndSchemaInvalidObjects) {
  ProductHistory history(Declaration(), 3);
  auto writer = history.TakeWriter();

  EXPECT_THROW(writer.Append(Time{} + 1s, 1), std::invalid_argument);
  EXPECT_THROW(writer.Append(Time{} + 1s, nlohmann::json::array({1})), std::invalid_argument);
  EXPECT_THROW(writer.Append(Time{} + 1s, {{"unexpected", true}}), std::invalid_argument);
  const auto valid = writer.Append(Time{} + 1s, {{"label", "cup"}});
  EXPECT_EQ(valid->sequence, 1u);
  EXPECT_EQ(valid->value.at("label"), "cup");
}

TEST(ProductHistoryTest, SupportsExactAndRelativeTimeLookups) {
  ProductHistory history(Declaration(), 8);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  const auto one = writer.Append(Time{} + 1s, {{"value", 1}});
  writer.Append(Time{} + 2s, {{"value", 2}});
  const auto duplicate = writer.Append(Time{} + 2s, {{"value", 3}});
  const auto four = writer.Append(Time{} + 4s, {{"value", 4}});

  EXPECT_EQ(reader.AtSequence(1).product, one);
  EXPECT_EQ(reader.AtTime(Time{} + 2s).product, duplicate);
  EXPECT_EQ(reader.Before(Time{} + 2s).product, one);
  EXPECT_EQ(reader.After(Time{} + 2s).product, four);
  EXPECT_EQ(reader.Nearest(Time{} + 3s).product, duplicate);  // Equal distance chooses earlier.
  EXPECT_FALSE(reader.AtTime(Time{} + 3s).product);
}

TEST(ProductHistoryTest, BoundsRangesAndReportsRetentionGaps) {
  ProductHistory history(Declaration(), 3);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  for (int second = 1; second <= 5; ++second)
    writer.Append(Time{} + second * 1s, {{"value", second}});

  const auto expired = reader.AtSequence(2);
  EXPECT_FALSE(expired.product);
  EXPECT_TRUE(expired.retention_gap);
  EXPECT_EQ(expired.retention.first_available_sequence, 3u);
  EXPECT_EQ(expired.retention.latest_sequence, 5u);
  EXPECT_EQ(expired.retention.evicted_through_sequence, 2u);

  const auto by_sequence = reader.RangeBySequence(1, 5, 2);
  ASSERT_EQ(by_sequence.products.size(), 2u);
  EXPECT_EQ(by_sequence.products[0]->sequence, 3u);
  EXPECT_EQ(by_sequence.products[1]->sequence, 4u);
  EXPECT_TRUE(by_sequence.retention_gap);
  EXPECT_TRUE(by_sequence.truncated);
  EXPECT_THROW(reader.RangeBySequence(0, 5, 2), std::invalid_argument);

  const auto by_time = reader.RangeByTime(Time{} + 1s, Time{} + 5s, 8);
  ASSERT_EQ(by_time.products.size(), 3u);
  EXPECT_TRUE(by_time.retention_gap);
  EXPECT_FALSE(by_time.truncated);
  EXPECT_THROW(reader.RangeBySequence(1, 2, 0), std::invalid_argument);
}

TEST(ProductHistoryTest, WaitsForTheFirstNewerProduct) {
  ProductHistory history(Declaration(), 4);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  const auto first = writer.Append(Time{} + 1s, {{"value", 1}});

  auto waiting = std::async(std::launch::async, [reader] { return reader.WaitNewer(1); });
  const auto second = writer.Append(Time{} + 2s, {{"value", 2}});
  const auto result = waiting.get();
  EXPECT_EQ(result.status, WaitStatus::kProduct);
  EXPECT_EQ(result.product, second);
  EXPECT_NE(result.product, first);
}

TEST(ProductHistoryTest, WaitCancellationAndCloseWakeReaders) {
  ProductHistory history(Declaration(), 2);
  auto reader = history.Reader();
  history.TakeWriter();
  std::atomic<bool> cancelled{false};
  auto cancelled_wait = std::async(std::launch::async, [reader, &cancelled] {
    return reader.WaitNewer(0, [&cancelled] { return cancelled.load(); });
  });
  cancelled.store(true);
  EXPECT_EQ(cancelled_wait.get().status, WaitStatus::kCancelled);

  auto closed_wait = std::async(std::launch::async, [reader] { return reader.WaitNewer(0); });
  history.Close();
  const auto closed = closed_wait.get();
  EXPECT_EQ(closed.status, WaitStatus::kClosed);
  EXPECT_TRUE(closed.retention.closed);
}

TEST(ProductHistoryTest, ConcurrentReadersObserveConsistentImmutableProducts) {
  ProductHistory history(Declaration(), 64);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  constexpr std::uint64_t kCount = 1000;
  auto consumer = std::async(std::launch::async, [reader] {
    std::uint64_t sequence = 0;
    while (sequence < kCount) {
      const auto next = reader.WaitNewer(sequence);
      EXPECT_EQ(next.status, WaitStatus::kProduct);
      if (!next.product) return sequence;
      EXPECT_GT(next.product->sequence, sequence);
      EXPECT_EQ(next.product->value.at("sequence"), next.product->sequence);
      sequence = next.product->sequence;
    }
    return sequence;
  });
  for (std::uint64_t sequence = 1; sequence <= kCount; ++sequence)
    writer.Append(Time{} + std::chrono::nanoseconds(sequence), {{"sequence", sequence}});
  EXPECT_EQ(consumer.get(), kCount);
}

TEST(ProductHistoryTest, CloseRejectsFurtherPublishesButRetainedProductsRemainReadable) {
  ProductHistory history(Declaration(), 2);
  auto reader = history.Reader();
  auto writer = history.TakeWriter();
  writer.Append(Time{} + 1s, {{"value", 1}});
  history.Close();
  EXPECT_THROW(writer.Append(Time{} + 2s, {{"value", 2}}), std::logic_error);
  EXPECT_EQ(reader.Latest().product->sequence, 1u);
  EXPECT_TRUE(reader.Latest().retention.closed);
}

}  // namespace
}  // namespace eos::runtime::perception

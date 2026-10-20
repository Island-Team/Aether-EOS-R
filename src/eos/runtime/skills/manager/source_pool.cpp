#include "src/eos/runtime/skills/manager/source_pool.h"
#include <algorithm>
#include <limits>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>
#include "src/eos/lib/uuid.h"
namespace eos::runtime::skills {
SourcePool::SourcePool(std::size_t limit, const eos::lib::Clock& clock, std::string boot_id)
    : limit_(limit), clock_(clock), boot_id_(std::move(boot_id)) {
  if (limit == 0) throw std::invalid_argument("source timeline limit must be greater than zero");
}
void SourcePool::Declare(const SourceDescriptor& descriptor) {
  nlohmann::json_schema::json_validator validator(nullptr,
      nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(descriptor.schema);
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = topics_.try_emplace(descriptor.topic, Timeline{descriptor.schema, {}});
  if (!inserted && it->second.schema != descriptor.schema)
    throw std::invalid_argument("conflicting source topic schemas");
}
SourceSample SourcePool::Publish(const std::string& topic, std::int64_t timestamp,
                                 nlohmann::json value) {
  if (!value.is_object()) throw std::invalid_argument("source sample must be a JSON object");
  if (timestamp < 0) throw std::invalid_argument("source timestamp must not be negative");
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = topics_.find(topic);
  if (it == topics_.end()) throw std::invalid_argument("source topic is not declared");
  nlohmann::json_schema::json_validator validator(nullptr,
      nlohmann::json_schema::default_string_format_check);
  validator.set_root_schema(it->second.schema);
  validator.validate(value);
  SourceSample sample{topic, timestamp, ++sequence_, std::move(value)};
  sample.event_time.source_timestamp_ns = timestamp;
  sample.event_time.source_clock_id = "device:" + topic;
  sample.event_time.observed_at = clock_.WallNow();
  sample.event_time.observed_monotonic_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.MonotonicNow().time_since_epoch())
          .count();
  sample.event_time.boot_id = boot_id_;
  sample.event_time.sequence = sample.sequence;
  sample.event_time.event_id = lib::MintUuidV7();
  it->second.samples.push_back(sample);
  while (it->second.samples.size() > limit_) it->second.samples.pop_front();
  changed_.notify_all();
  return sample;
}
std::optional<SourceSample> SourcePool::Query(const std::string& topic, SourceLookup lookup,
                                               std::int64_t timestamp) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = topics_.find(topic);
  if (it == topics_.end()) throw std::invalid_argument("source topic is not declared");
  if (it->second.samples.empty()) return std::nullopt;
  if (lookup == SourceLookup::kLatest) return it->second.samples.back();
  if (timestamp < 0) throw std::invalid_argument("source timestamp must not be negative");
  const SourceSample* best = nullptr;
  std::uint64_t distance = std::numeric_limits<std::uint64_t>::max();
  for (const auto& sample : it->second.samples) {
    if (lookup == SourceLookup::kExact && sample.source_timestamp_ns == timestamp &&
        (best == nullptr || sample.sequence > best->sequence)) best = &sample;
    if (lookup == SourceLookup::kBefore && sample.source_timestamp_ns <= timestamp &&
        (best == nullptr || sample.source_timestamp_ns > best->source_timestamp_ns ||
         (sample.source_timestamp_ns == best->source_timestamp_ns && sample.sequence > best->sequence))) best = &sample;
    if (lookup == SourceLookup::kAfter && sample.source_timestamp_ns >= timestamp &&
        (best == nullptr || sample.source_timestamp_ns < best->source_timestamp_ns ||
         (sample.source_timestamp_ns == best->source_timestamp_ns && sample.sequence < best->sequence))) best = &sample;
    if (lookup == SourceLookup::kNearest) {
      const std::uint64_t current = sample.source_timestamp_ns >= timestamp
          ? static_cast<std::uint64_t>(sample.source_timestamp_ns) -
                static_cast<std::uint64_t>(timestamp)
          : static_cast<std::uint64_t>(timestamp) -
                static_cast<std::uint64_t>(sample.source_timestamp_ns);
      if (best == nullptr || current < distance || (current == distance && sample.sequence > best->sequence)) {
        best = &sample; distance = current;
      }
    }
  }
  return best == nullptr ? std::nullopt : std::optional<SourceSample>(*best);
}
std::optional<SourceSample> SourcePool::WaitForNewer(const std::string& topic,
    std::uint64_t after, std::chrono::milliseconds timeout,
    const std::function<bool()>& cancelled) const {
  std::unique_lock<std::mutex> lock(mutex_);
  if (topics_.count(topic) == 0) throw std::invalid_argument("source topic is not declared");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto& samples = topics_.at(topic).samples;
    auto it = std::find_if(samples.begin(), samples.end(),
                           [after](const SourceSample& sample) { return sample.sequence > after; });
    if (it != samples.end()) return *it;
    if (closed_ || (cancelled && cancelled()) || std::chrono::steady_clock::now() >= deadline)
      return std::nullopt;
    changed_.wait_for(lock, std::chrono::milliseconds(20));
  }
}
void SourcePool::Close() { std::lock_guard<std::mutex> lock(mutex_); closed_ = true; changed_.notify_all(); }
}  // namespace eos::runtime::skills

#include "src/eos/runtime/skills/manager/journald.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <openssl/sha.h>

#include "src/eos/lib/log.h"
#include "src/eos/lib/uuid.h"

namespace eos::runtime::skills {
namespace {
void Trim(detail::Record& record, std::size_t limit) {
  while (record.events.size() > limit) record.events.pop_front();
  while (record.lifecycle.size() > limit) record.lifecycle.erase(record.lifecycle.begin());
}

std::string Timestamp(std::chrono::system_clock::time_point value) {
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                value.time_since_epoch()) %
                            1000;
  const std::time_t seconds = std::chrono::system_clock::to_time_t(value);
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
         << std::setw(3) << milliseconds.count() << 'Z';
  return output.str();
}

const char* RecordKindName(JournalKind kind) {
  switch (kind) {
    case JournalKind::kActivity: return "activity";
    case JournalKind::kCheckpoint: return "checkpoint";
    case JournalKind::kSuppression: return "suppression";
  }
  return "unknown";
}

std::string Sha256(std::string_view input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned char byte : digest) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

nlohmann::json PayloadMetadata(const nlohmann::json& payload) {
  const std::string encoded = payload.dump();
  return {{"size_bytes", encoded.size()}, {"sha256", Sha256(encoded)}};
}

bool ValidFrame(const nlohmann::json& value) {
  if (!value.is_object() || !value.contains("frame_sha256") ||
      !value.at("frame_sha256").is_string())
    return false;
  const std::string expected = value.at("frame_sha256").get<std::string>();
  auto body = value;
  body.erase("frame_sha256");
  return expected.size() == 64 && Sha256(body.dump()) == expected;
}

bool MatchesQuery(const nlohmann::json& value, const DiagnosticQuery& query) {
  const auto matches = [&](const char* key, const std::string& expected) {
    return expected.empty() ||
           (value.contains(key) && value.at(key).is_string() && value.at(key) == expected);
  };
  if (!matches("execution_id", query.execution_id) ||
      !matches("skill_name", query.skill_name) || !matches("operation_id", query.operation_id) ||
      !matches("correlation_id", query.correlation_id) || !matches("boot_id", query.boot_id) ||
      !matches("severity", query.severity))
    return false;
  const std::string timestamp = value.value("timestamp", std::string());
  return (query.from_timestamp.empty() || timestamp >= query.from_timestamp) &&
         (query.to_timestamp.empty() || timestamp <= query.to_timestamp);
}
}  // namespace

SkillJournald::SkillJournald(std::size_t max_monitor_updates,
                             std::optional<std::filesystem::path> log_directory, bool overwrite,
                             std::uint64_t rotation_size_bytes, std::uint32_t rotation_files)
    : SkillJournald(max_monitor_updates, []() -> const eos::lib::Clock& {
        static const eos::lib::SystemClock clock;
        return clock;
      }(), lib::MintUuidV7(), std::move(log_directory), overwrite, rotation_size_bytes,
      rotation_files) {}

SkillJournald::SkillJournald(std::size_t max_monitor_updates,
                             const eos::lib::Clock& clock, std::string boot_id,
                             std::optional<std::filesystem::path> log_directory, bool overwrite,
                             std::uint64_t rotation_size_bytes, std::uint32_t rotation_files)
    : max_monitor_updates_(max_monitor_updates),
      overwrite_(overwrite),
      rotation_size_bytes_(rotation_size_bytes),
      rotation_files_(rotation_files),
      clock_(clock),
      boot_id_(std::move(boot_id)) {
  if (max_monitor_updates_ == 0)
    throw std::invalid_argument("monitor history limit must be positive");
  if (rotation_size_bytes_ == 0 || rotation_files_ == 0)
    throw std::invalid_argument("journal log rotation limits must be positive");
  if (log_directory) log_path_ = *log_directory / "skill-diagnostics.jsonl";
  persistence_status_.enabled = log_path_.has_value();
  if (log_path_) persistence_thread_ = std::thread(&SkillJournald::PersistenceLoop, this);
}

SkillJournald::~SkillJournald() {
  if (!persistence_thread_.joinable()) return;
  {
    std::lock_guard<std::mutex> lock(persistence_mutex_);
    persistence_stopping_ = true;
  }
  persistence_changed_.notify_all();
  persistence_thread_.join();
}

void SkillJournald::AppendLifecycle(detail::Record& record, std::size_t max_entries) const {
  ++record.snapshot.sequence;
  ++record.snapshot.latest_journal_sequence;
  JournalEntry entry;
  entry.sequence = record.snapshot.latest_journal_sequence;
  entry.timestamp = clock_.WallNow();
  record.snapshot.event_time.observed_at = entry.timestamp;
  record.snapshot.event_time.observed_monotonic_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock_.MonotonicNow().time_since_epoch())
          .count();
  record.snapshot.event_time.boot_id = boot_id_;
  record.snapshot.event_time.sequence = entry.sequence;
  record.snapshot.event_time.event_id = lib::MintUuidV7();
  record.lifecycle.push_back(record.snapshot);
  entry.kind = JournalEntry::Kind::kLifecycle;
  entry.execution = record.snapshot;
  record.events.push_back(std::move(entry));
  Trim(record, max_entries);
  Persist(record.events.back());
}

bool SkillJournald::AppendSignal(detail::Record& record, Signal signal,
                                std::chrono::system_clock::time_point timestamp,
                                std::size_t max_entries) const {
  if (record.events.size() >= max_entries) return false;
  ++record.snapshot.latest_journal_sequence;
  JournalEntry entry;
  entry.sequence = record.snapshot.latest_journal_sequence;
  entry.timestamp = timestamp;
  entry.kind = JournalEntry::Kind::kSignal;
  entry.execution = record.snapshot;
  entry.signal = std::move(signal);
  record.events.push_back(std::move(entry));
  Persist(record.events.back());
  return true;
}

bool SkillJournald::AppendRecord(detail::Record& record, JournalRecord journal_record,
                                std::size_t max_entries) const {
  if (record.events.size() >= max_entries) return false;
  ++record.snapshot.latest_journal_sequence;
  JournalEntry entry;
  entry.sequence = record.snapshot.latest_journal_sequence;
  entry.timestamp = journal_record.timestamp;
  entry.kind = JournalEntry::Kind::kRecord;
  entry.execution = record.snapshot;
  entry.record = std::move(journal_record);
  record.events.push_back(std::move(entry));
  Persist(record.events.back());
  return true;
}

void SkillJournald::Persist(const JournalEntry& entry) const noexcept {
  if (!log_path_) return;
  try {
    nlohmann::json value = {{"timestamp", Timestamp(entry.timestamp)},
                            {"sequence", entry.sequence},
                            {"execution_id", entry.execution.execution_id},
                            {"request_id", entry.execution.request_id},
                            {"skill_name", entry.execution.skill_name},
                            {"skill_version", entry.execution.skill_version},
                            {"runtime_id", entry.execution.runtime_id},
                            {"boot_id", entry.execution.boot_id},
                            {"severity", entry.execution.status == ExecutionStatus::kFailed
                                             ? "error"
                                             : "info"},
                            {"status", ExecutionStatusName(entry.execution.status)},
                            {"execution_sequence", entry.execution.sequence}};
    if (entry.kind == JournalEntry::Kind::kLifecycle) {
      value["type"] = "lifecycle";
      value["reason_code"] = entry.execution.reason_code;
      value["message"] = entry.execution.message;
      value["output_summary"] = PayloadMetadata(entry.execution.result);
    } else if (entry.kind == JournalEntry::Kind::kSignal) {
      value["type"] = "signal";
      value["signal"] = {{"name", entry.signal.name},
                         {"signal_id", entry.signal.signal_id},
                         {"value_summary", PayloadMetadata(entry.signal.payload)}};
    } else {
      value["type"] = "record";
      value["record"] = {
          {"event_id", entry.record.event_id},
          {"source", entry.record.source == JournalSource::kSkill ? "skill" : "system"},
          {"kind", RecordKindName(entry.record.kind)},
          {"name", entry.record.name},
          {"outcome", entry.record.outcome},
          {"message", entry.record.message},
          {"fields", entry.record.fields},
          {"elapsed_ms", entry.record.elapsed.count()}};
      if (entry.record.progress) value["record"]["progress"] = *entry.record.progress;
      if (entry.record.duration)
        value["record"]["duration_ms"] = entry.record.duration->count();
    }
    value = eos::lib::SanitizeLogFields(value);
    value["frame_sha256"] = Sha256(value.dump());
    const std::string line = value.dump() + "\n";
    std::lock_guard<std::mutex> lock(persistence_mutex_);
    constexpr std::size_t kMaximumQueuedRecords = 4096;
    if (persistence_queue_.size() >= kMaximumQueuedRecords) {
      ++persistence_status_.records_dropped;
      persistence_status_.healthy = false;
      persistence_status_.last_error = "diagnostic persistence queue is full";
      return;
    }
    persistence_queue_.push_back(line);
    persistence_changed_.notify_one();
  } catch (...) {
    std::lock_guard<std::mutex> lock(persistence_mutex_);
    ++persistence_status_.records_dropped;
    persistence_status_.healthy = false;
    persistence_status_.last_error = "failed to encode diagnostic record";
  }
}

bool SkillJournald::WriteLine(const std::string& line) noexcept {
  try {
    std::string preparation_error;
    if (!persistence_initialized_) {
      if (!PreparePersistence(preparation_error)) throw std::runtime_error(preparation_error);
      persistence_initialized_ = true;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(*log_path_, error);
    if (!error && size > 0 && size + line.size() > rotation_size_bytes_) {
      std::filesystem::remove(log_path_->string() + "." + std::to_string(rotation_files_), error);
      for (std::uint32_t index = rotation_files_; index > 1; --index) {
        const auto from = log_path_->string() + "." + std::to_string(index - 1);
        const auto to = log_path_->string() + "." + std::to_string(index);
        if (std::filesystem::exists(from, error)) std::filesystem::rename(from, to, error);
      }
      std::filesystem::rename(*log_path_, log_path_->string() + ".1", error);
    }
    std::ofstream output(*log_path_, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error("cannot open diagnostic journal");
    output << line;
    output.flush();
    if (!output) throw std::runtime_error("cannot write diagnostic journal");
    return true;
  } catch (...) {
    return false;
  }
}

bool SkillJournald::PreparePersistence(std::string& message) noexcept {
  try {
    std::error_code error;
    std::filesystem::create_directories(log_path_->parent_path(), error);
    if (error) throw std::runtime_error(error.message());
    if (overwrite_) {
      std::filesystem::remove(*log_path_, error);
      for (std::uint32_t index = 1; index <= rotation_files_; ++index)
        std::filesystem::remove(log_path_->string() + "." + std::to_string(index), error);
      return true;
    }
    if (!std::filesystem::exists(*log_path_)) return true;
    std::ifstream input(*log_path_, std::ios::binary);
    if (!input) throw std::runtime_error("cannot inspect diagnostic journal");
    std::uintmax_t valid_bytes = 0;
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty()) {
        valid_bytes = static_cast<std::uintmax_t>(input.tellg());
        continue;
      }
      try {
        if (!ValidFrame(nlohmann::json::parse(line))) break;
      } catch (...) {
        break;
      }
      const auto position = input.tellg();
      valid_bytes = position < 0 ? std::filesystem::file_size(*log_path_) :
                                   static_cast<std::uintmax_t>(position);
    }
    const auto size = std::filesystem::file_size(*log_path_);
    if (valid_bytes < size) std::filesystem::resize_file(*log_path_, valid_bytes);
    return true;
  } catch (const std::exception& error) {
    message = error.what();
    return false;
  } catch (...) {
    message = "unknown persistence preparation error";
    return false;
  }
}

std::vector<nlohmann::json> ReadSkillDiagnostics(const std::filesystem::path& path,
                                                 const DiagnosticQuery& query) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open skill diagnostic journal");
  std::vector<nlohmann::json> records;
  for (std::string line; std::getline(input, line);) {
    if (line.empty()) continue;
    nlohmann::json value;
    try {
      value = nlohmann::json::parse(line);
    } catch (...) {
      break;
    }
    if (!ValidFrame(value)) break;
    if (MatchesQuery(value, query)) records.push_back(std::move(value));
  }
  return records;
}

void SkillJournald::PersistenceLoop() {
  while (true) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(persistence_mutex_);
      persistence_changed_.wait(lock,
                                [&] { return persistence_stopping_ || !persistence_queue_.empty(); });
      if (persistence_queue_.empty() && persistence_stopping_) break;
      line = std::move(persistence_queue_.front());
      persistence_queue_.pop_front();
      persistence_writing_ = true;
    }
    const bool written = WriteLine(line);
    {
      std::lock_guard<std::mutex> lock(persistence_mutex_);
      persistence_writing_ = false;
      if (written) {
        ++persistence_status_.records_written;
      } else {
        ++persistence_status_.records_dropped;
        persistence_status_.healthy = false;
        persistence_status_.last_error = "failed to write diagnostic journal";
      }
      if (persistence_queue_.empty()) persistence_flushed_.notify_all();
    }
  }
}

void SkillJournald::Flush() const {
  std::unique_lock<std::mutex> lock(persistence_mutex_);
  persistence_flushed_.wait(lock,
                            [&] { return persistence_queue_.empty() && !persistence_writing_; });
}

SkillJournald::PersistenceStatus SkillJournald::persistence_status() const {
  std::lock_guard<std::mutex> lock(persistence_mutex_);
  return persistence_status_;
}

void SkillJournald::Publish(const ExecutionSnapshot& snapshot) {
  std::lock_guard<std::mutex> lock(monitor_mutex_);
  Delta delta;
  delta.sequence = ++monitor_sequence_;
  if (const auto found = current_.find(snapshot.execution_id); found != current_.end())
    delta.before = found->second;
  delta.after = snapshot;
  current_[snapshot.execution_id] = snapshot;
  monitor_history_.push_back(std::move(delta));
  while (monitor_history_.size() > max_monitor_updates_) monitor_history_.pop_front();
}

void SkillJournald::Remove(const std::string& execution_id) {
  std::lock_guard<std::mutex> lock(monitor_mutex_);
  const auto found = current_.find(execution_id);
  if (found == current_.end()) return;
  Delta delta;
  delta.sequence = ++monitor_sequence_;
  delta.before = found->second;
  current_.erase(found);
  monitor_history_.push_back(std::move(delta));
  while (monitor_history_.size() > max_monitor_updates_) monitor_history_.pop_front();
}

bool SkillJournald::Matches(const ExecutionSnapshot& snapshot, const ExecutionSelector& selector) {
  if (!selector.skill_name.empty() && snapshot.skill_name != selector.skill_name) return false;
  if (!selector.skill_version.empty() && snapshot.skill_version != selector.skill_version)
    return false;
  if (selector.statuses.empty())
    return snapshot.status == ExecutionStatus::kQueued || snapshot.status == ExecutionStatus::kRunning;
  return std::find(selector.statuses.begin(), selector.statuses.end(), snapshot.status) !=
         selector.statuses.end();
}

MonitorUpdate SkillJournald::Reset(const ExecutionSelector& selector) const {
  MonitorUpdate update;
  update.sequence = monitor_sequence_;
  update.reset = true;
  for (const auto& [_, snapshot] : current_)
    if (Matches(snapshot, selector)) update.upserted.push_back(snapshot);
  std::sort(update.upserted.begin(), update.upserted.end(), [](const auto& a, const auto& b) {
    return a.execution_id < b.execution_id;
  });
  return update;
}

std::vector<MonitorUpdate> SkillJournald::ReadMonitor(const ExecutionSelector& selector,
                                                      std::uint64_t after) const {
  std::lock_guard<std::mutex> lock(monitor_mutex_);
  if (!selector.skill_version.empty() && selector.skill_name.empty())
    throw std::invalid_argument("skill version requires skill name");
  if (after > monitor_sequence_) throw std::invalid_argument("monitor cursor is ahead of head");
  if (after == 0 || (!monitor_history_.empty() && after + 1 < monitor_history_.front().sequence))
    return {Reset(selector)};
  std::vector<MonitorUpdate> result;
  std::uint64_t scanned = after;
  for (const auto& delta : monitor_history_) {
    if (delta.sequence <= after) continue;
    scanned = delta.sequence;
    const bool before = delta.before && Matches(*delta.before, selector);
    const bool current = delta.after && Matches(*delta.after, selector);
    if (!before && !current) continue;
    MonitorUpdate update;
    update.sequence = delta.sequence;
    if (current)
      update.upserted.push_back(*delta.after);
    else
      update.removed_execution_ids.push_back(delta.before->execution_id);
    result.push_back(std::move(update));
  }
  if (scanned > after && (result.empty() || result.back().sequence < scanned)) {
    MonitorUpdate cursor_advance;
    cursor_advance.sequence = scanned;
    result.push_back(std::move(cursor_advance));
  }
  return result;
}

std::uint64_t SkillJournald::monitor_sequence() const {
  std::lock_guard<std::mutex> lock(monitor_mutex_);
  return monitor_sequence_;
}

}  // namespace eos::runtime::skills

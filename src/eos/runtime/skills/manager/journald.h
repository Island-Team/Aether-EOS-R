#pragma once

#include <deque>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "src/eos/runtime/skills/manager/record_store.h"

namespace eos::runtime::skills {

struct DiagnosticQuery {
  std::string execution_id;
  std::string skill_name;
  std::string operation_id;
  std::string correlation_id;
  std::string boot_id;
  std::string severity;
  std::string from_timestamp;
  std::string to_timestamp;
};

// Reads only complete, checksum-valid diagnostic records. Empty query fields
// are wildcards; timestamps compare lexically in the journal's UTC format.
std::vector<nlohmann::json> ReadSkillDiagnostics(const std::filesystem::path&,
                                                 const DiagnosticQuery& = {});

/// Process-local journal and logical-process monitor owned by SkillManager.
/// The manager mutex serializes all monitor methods; per-record journal methods
/// require the corresponding record mutex.
class SkillJournald final {
 public:
  struct PersistenceStatus {
    bool enabled{false};
    bool healthy{true};
    std::uint64_t records_written{0};
    std::uint64_t records_dropped{0};
    std::string last_error;
  };
  explicit SkillJournald(std::size_t max_monitor_updates,
                         std::optional<std::filesystem::path> log_directory = std::nullopt,
                         bool overwrite = false,
                         std::uint64_t rotation_size_bytes = 64ULL * 1024ULL * 1024ULL,
                         std::uint32_t rotation_files = 5);
  ~SkillJournald();
  explicit SkillJournald(std::size_t max_monitor_updates,
                         const eos::lib::Clock& clock, std::string boot_id,
                         std::optional<std::filesystem::path> log_directory = std::nullopt,
                         bool overwrite = false,
                         std::uint64_t rotation_size_bytes = 64ULL * 1024ULL * 1024ULL,
                         std::uint32_t rotation_files = 5);

  void AppendLifecycle(detail::Record&, std::size_t max_entries) const;
  bool AppendSignal(detail::Record&, Signal, std::chrono::system_clock::time_point,
                    std::size_t max_entries) const;
  bool AppendRecord(detail::Record&, JournalRecord, std::size_t max_entries) const;

  void Publish(const ExecutionSnapshot&);
  void Remove(const std::string& execution_id);
  std::uint64_t monitor_sequence() const;
  std::vector<MonitorUpdate> ReadMonitor(const ExecutionSelector&, std::uint64_t after) const;

  static bool Matches(const ExecutionSnapshot&, const ExecutionSelector&);
  PersistenceStatus persistence_status() const;
  void Flush() const;

 private:
  struct Delta {
    std::uint64_t sequence{0};
    std::optional<ExecutionSnapshot> before;
    std::optional<ExecutionSnapshot> after;
  };

  MonitorUpdate Reset(const ExecutionSelector&) const;
  void Persist(const JournalEntry&) const noexcept;
  void PersistenceLoop();
  bool WriteLine(const std::string&) noexcept;
  bool PreparePersistence(std::string&) noexcept;
  std::size_t max_monitor_updates_;
  std::optional<std::filesystem::path> log_path_;
  bool overwrite_{false};
  std::uint64_t rotation_size_bytes_;
  std::uint32_t rotation_files_;
  mutable std::mutex persistence_mutex_;
  mutable std::condition_variable persistence_changed_;
  mutable std::condition_variable persistence_flushed_;
  mutable std::deque<std::string> persistence_queue_;
  mutable bool persistence_stopping_{false};
  mutable bool persistence_writing_{false};
  mutable PersistenceStatus persistence_status_;
  std::thread persistence_thread_;
  mutable bool persistence_initialized_{false};
  const eos::lib::Clock& clock_;
  std::string boot_id_;
  mutable std::mutex monitor_mutex_;
  std::uint64_t monitor_sequence_{0};
  std::deque<Delta> monitor_history_;
  std::unordered_map<std::string, ExecutionSnapshot> current_;
};

}  // namespace eos::runtime::skills

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace eos::runtime::actionlist {

/// Versioned, append-only storage for authoritative ActionList mutations.
class Journal final {
 public:
  struct Limits {
    std::size_t maximum_record_bytes{4U * 1024U * 1024U};
    std::uint64_t maximum_journal_bytes{256ULL * 1024ULL * 1024ULL};
  };

  explicit Journal(std::filesystem::path directory);
  Journal(std::filesystem::path directory, Limits limits);

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  /// Appends and synchronizes one typed JSON payload.
  void Append(const std::string& kind, const nlohmann::json& payload);
  /// Atomically replaces the journal with the supplied authoritative records.
  void Rewrite(const std::vector<nlohmann::json>& records);
  /// Replays complete records and removes only an invalid final record.
  std::vector<nlohmann::json> Replay();

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
  Limits limits_;
};

}  // namespace eos::runtime::actionlist

/// Atomic v2 map and source-library persistence for the standalone simulator.
#pragma once
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>
namespace eos::ext::simulator {
class MapStore {
 public:
  explicit MapStore(std::filesystem::path state_root);
  nlohmann::json List() const;
  nlohmann::json ReadMetadata(const std::string& id) const;
  std::vector<std::uint8_t> ReadRaster(const std::string& id) const;
  nlohmann::json Create(const nlohmann::json& metadata);
  nlohmann::json ReplaceMetadata(const std::string& id, const nlohmann::json& metadata,
                                 std::uint64_t expected_revision);
  nlohmann::json ReplaceRaster(const std::string& id, const std::vector<std::uint8_t>& png,
                               std::uint64_t expected_revision);
  nlohmann::json ReplaceBundle(const std::string& id, const nlohmann::json& metadata,
                               const std::vector<std::uint8_t>& png,
                               std::uint64_t expected_revision);
  void Remove(const std::string& id, std::uint64_t expected_revision);
  const std::filesystem::path& root() const { return root_; }
  static void ValidateId(const std::string& id);

 private:
  std::filesystem::path Bundle(const std::string& id) const;
  void ValidateSources(const nlohmann::json& metadata) const;
  std::filesystem::path root_;
  std::filesystem::path library_index_;
};

class SourceLibrary {
 public:
  explicit SourceLibrary(std::filesystem::path state_root);
  nlohmann::json List() const;
  nlohmann::json ReadMetadata(const std::string& id) const;
  std::pair<std::string, std::vector<std::uint8_t>> Read(const std::string& id) const;
  nlohmann::json Write(const std::string& id, const std::string& media_type,
                       const std::vector<std::uint8_t>& bytes, std::uint64_t expected_revision);
  void Remove(const std::string& id, std::uint64_t expected_revision);
  std::filesystem::path Resolve(const std::string& id) const;
  const std::filesystem::path& root() const { return root_; }

 private:
  nlohmann::json Index() const;
  void WriteIndex(const nlohmann::json& index) const;
  bool Referenced(const std::string& id) const;
  std::filesystem::path root_;
  std::filesystem::path maps_root_;
};
}  // namespace eos::ext::simulator

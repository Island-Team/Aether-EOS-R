#pragma once

#include <filesystem>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace eos::runtime::store {

struct NavigationPoint {
  std::string name;
  double x_m{0.0};
  double y_m{0.0};
  double yaw_rad{0.0};
};

class StoreError : public std::runtime_error {
 public:
  enum class Code { kInvalid, kNotFound, kConflict, kExhausted, kIo };
  StoreError(Code code, const std::string& message) : std::runtime_error(message), code_(code) {}
  Code code() const noexcept { return code_; }

 private:
  Code code_;
};

class NavigationPointStore final {
 public:
  explicit NavigationPointStore(std::filesystem::path directory);

  std::vector<NavigationPoint> List(const std::string& robot_name) const;
  NavigationPoint Get(const std::string& robot_name, const std::string& name) const;
  NavigationPoint Put(const std::string& robot_name, NavigationPoint point);
  void Delete(const std::string& robot_name, const std::string& name);
  std::size_t Reload();

  nlohmann::json ListResources() const;
  std::optional<nlohmann::json> ReadResource(const std::string& uri) const;
  bool SupportsResourceTemplate(const std::string& uri_template) const;
  const std::filesystem::path& path() const { return path_; }

 private:
  using Points = std::map<std::string, NavigationPoint>;
  using Robots = std::map<std::string, Points>;

  static void ValidateSegment(const std::string& value, const char* label);
  static void ValidatePoint(const NavigationPoint& point);
  static Robots Decode(const std::string& encoded);
  static std::string Encode(const Robots& robots);
  std::string ReadRaw() const;
  void Persist(const std::string& encoded) const;
  void RequireUnchanged() const;

  std::filesystem::path directory_;
  std::filesystem::path path_;
  mutable std::mutex mutex_;
  Robots robots_;
  std::string loaded_bytes_;
};

}  // namespace eos::runtime::store

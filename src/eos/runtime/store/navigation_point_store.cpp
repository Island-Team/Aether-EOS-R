#include "src/eos/runtime/store/navigation_point_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace eos::runtime::store {
namespace {

constexpr std::size_t kMaximumFileBytes = 4U * 1024U * 1024U;
constexpr std::size_t kMaximumPoints = 10000;
std::atomic<std::uint64_t> temporary_sequence{0};

[[noreturn]] void IoError(const std::filesystem::path& path, const std::string& operation) {
  throw StoreError(StoreError::Code::kIo, "cannot " + operation + " navigation point store '" +
                                              path.string() + "': " + std::strerror(errno));
}

void WriteAll(int descriptor, const std::string& value, const std::filesystem::path& path) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const ssize_t written = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (written <= 0) IoError(path, "write");
    offset += static_cast<std::size_t>(written);
  }
}

void SyncDirectory(const std::filesystem::path& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) IoError(path, "open directory for");
  const bool synced = ::fsync(descriptor) == 0;
  const bool closed = ::close(descriptor) == 0;
  if (!synced || !closed) IoError(path, "sync directory for");
}

}  // namespace

NavigationPointStore::NavigationPointStore(std::filesystem::path directory)
    : directory_(std::move(directory)), path_(directory_ / "navigation-points.v1.json") {
  if (directory_.empty())
    throw StoreError(StoreError::Code::kInvalid, "store directory is required");
  std::error_code error;
  const bool created = std::filesystem::create_directories(directory_, error);
  if (error)
    throw StoreError(StoreError::Code::kIo, "cannot create store directory: " + error.message());
  if (::chmod(directory_.c_str(), S_IRWXU) != 0) IoError(directory_, "secure");
  if (created) SyncDirectory(directory_);
  loaded_bytes_ = ReadRaw();
  robots_ = Decode(loaded_bytes_);
}

void NavigationPointStore::ValidateSegment(const std::string& value, const char* label) {
  if (value.empty() || value.size() > 128 || value == "." || value == "..")
    throw StoreError(StoreError::Code::kInvalid, std::string(label) + " is invalid");
  for (const unsigned char byte : value) {
    if (byte == '/' || byte < 0x20 || byte == 0x7f)
      throw StoreError(StoreError::Code::kInvalid, std::string(label) + " is invalid");
  }
}

void NavigationPointStore::ValidatePoint(const NavigationPoint& point) {
  ValidateSegment(point.name, "navigation point name");
  if (!std::isfinite(point.x_m) || !std::isfinite(point.y_m) || !std::isfinite(point.yaw_rad))
    throw StoreError(StoreError::Code::kInvalid, "navigation point coordinates must be finite");
}

NavigationPointStore::Robots NavigationPointStore::Decode(const std::string& encoded) {
  if (encoded.empty()) return {};
  if (encoded.size() > kMaximumFileBytes)
    throw StoreError(StoreError::Code::kExhausted, "navigation point store exceeds 4 MiB");
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(encoded);
  } catch (const std::exception& error) {
    throw StoreError(StoreError::Code::kInvalid,
                     "navigation point store contains invalid JSON: " + std::string(error.what()));
  }
  if (!root.is_object() || !root.contains("format_version") ||
      !root.at("format_version").is_number_integer() || root.at("format_version") != 1 ||
      !root.contains("robots") || !root.at("robots").is_object() || root.size() != 2)
    throw StoreError(StoreError::Code::kInvalid, "navigation point store envelope is invalid");
  Robots result;
  std::size_t count = 0;
  for (const auto& [robot_name, robot] : root.at("robots").items()) {
    ValidateSegment(robot_name, "robot name");
    if (!robot.is_object() || robot.size() != 1 || !robot.contains("navigation_points") ||
        !robot.at("navigation_points").is_object())
      throw StoreError(StoreError::Code::kInvalid, "robot navigation point table is invalid");
    auto& output = result[robot_name];
    for (const auto& [name, value] : robot.at("navigation_points").items()) {
      if (!value.is_object() || value.size() != 3 || !value.contains("x_m") ||
          !value.contains("y_m") || !value.contains("yaw_rad") || !value.at("x_m").is_number() ||
          !value.at("y_m").is_number() || !value.at("yaw_rad").is_number())
        throw StoreError(StoreError::Code::kInvalid, "navigation point row is invalid");
      NavigationPoint point{name, value.at("x_m").get<double>(), value.at("y_m").get<double>(),
                            value.at("yaw_rad").get<double>()};
      ValidatePoint(point);
      output.emplace(name, std::move(point));
      if (++count > kMaximumPoints)
        throw StoreError(StoreError::Code::kExhausted, "navigation point store exceeds 10000 rows");
    }
  }
  return result;
}

std::string NavigationPointStore::Encode(const Robots& robots) {
  nlohmann::json root{{"format_version", 1}, {"robots", nlohmann::json::object()}};
  for (const auto& [robot_name, points] : robots) {
    auto& table = root["robots"][robot_name]["navigation_points"];
    table = nlohmann::json::object();
    for (const auto& [name, point] : points)
      table[name] = {{"x_m", point.x_m}, {"y_m", point.y_m}, {"yaw_rad", point.yaw_rad}};
  }
  std::string encoded = root.dump(2) + "\n";
  if (encoded.size() > kMaximumFileBytes)
    throw StoreError(StoreError::Code::kExhausted, "navigation point store exceeds 4 MiB");
  return encoded;
}

std::string NavigationPointStore::ReadRaw() const {
  struct stat status{};
  if (::lstat(path_.c_str(), &status) != 0) {
    if (errno == ENOENT) return {};
    IoError(path_, "inspect");
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > kMaximumFileBytes)
    throw StoreError(StoreError::Code::kInvalid,
                     "navigation point store is not a bounded regular file");
  std::ifstream input(path_, std::ios::binary);
  if (!input) IoError(path_, "read");
  return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void NavigationPointStore::Persist(const std::string& encoded) const {
  const std::filesystem::path temporary =
      directory_ / (".navigation-points." + std::to_string(::getpid()) + "." +
                    std::to_string(temporary_sequence.fetch_add(1)) + ".tmp");
  const int descriptor = ::open(
      temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
  if (descriptor < 0) IoError(temporary, "create temporary");
  bool open = true;
  try {
    WriteAll(descriptor, encoded, temporary);
    if (::fsync(descriptor) != 0) IoError(temporary, "sync");
    if (::close(descriptor) != 0) IoError(temporary, "close");
    open = false;
    if (::rename(temporary.c_str(), path_.c_str()) != 0) IoError(path_, "replace");
    SyncDirectory(directory_);
  } catch (...) {
    if (open) (void)::close(descriptor);
    (void)::unlink(temporary.c_str());
    throw;
  }
}

void NavigationPointStore::RequireUnchanged() const {
  if (ReadRaw() != loaded_bytes_)
    throw StoreError(StoreError::Code::kConflict,
                     "navigation point file changed externally; reload before writing");
}

std::vector<NavigationPoint> NavigationPointStore::List(const std::string& robot_name) const {
  ValidateSegment(robot_name, "robot name");
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<NavigationPoint> result;
  const auto robot = robots_.find(robot_name);
  if (robot != robots_.end())
    for (const auto& [name, point] : robot->second) result.push_back(point);
  return result;
}

NavigationPoint NavigationPointStore::Get(const std::string& robot_name,
                                          const std::string& name) const {
  ValidateSegment(robot_name, "robot name");
  ValidateSegment(name, "navigation point name");
  std::lock_guard<std::mutex> lock(mutex_);
  const auto robot = robots_.find(robot_name);
  if (robot == robots_.end() || robot->second.count(name) == 0)
    throw StoreError(StoreError::Code::kNotFound, "navigation point not found");
  return robot->second.at(name);
}

NavigationPoint NavigationPointStore::Put(const std::string& robot_name, NavigationPoint point) {
  ValidateSegment(robot_name, "robot name");
  ValidatePoint(point);
  std::lock_guard<std::mutex> lock(mutex_);
  RequireUnchanged();
  Robots next = robots_;
  next[robot_name].insert_or_assign(point.name, point);
  std::size_t count = 0;
  for (const auto& [robot, points] : next) count += points.size();
  if (count > kMaximumPoints)
    throw StoreError(StoreError::Code::kExhausted, "navigation point store exceeds 10000 rows");
  std::string encoded = Encode(next);
  Persist(encoded);
  robots_ = std::move(next);
  loaded_bytes_ = std::move(encoded);
  return point;
}

void NavigationPointStore::Delete(const std::string& robot_name, const std::string& name) {
  ValidateSegment(robot_name, "robot name");
  ValidateSegment(name, "navigation point name");
  std::lock_guard<std::mutex> lock(mutex_);
  RequireUnchanged();
  Robots next = robots_;
  auto robot = next.find(robot_name);
  if (robot == next.end() || robot->second.erase(name) == 0)
    throw StoreError(StoreError::Code::kNotFound, "navigation point not found");
  if (robot->second.empty()) next.erase(robot);
  std::string encoded = Encode(next);
  Persist(encoded);
  robots_ = std::move(next);
  loaded_bytes_ = std::move(encoded);
}

std::size_t NavigationPointStore::Reload() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string encoded = ReadRaw();
  Robots next = Decode(encoded);
  std::size_t count = 0;
  for (const auto& [robot, points] : next) count += points.size();
  robots_ = std::move(next);
  loaded_bytes_ = std::move(encoded);
  return count;
}

nlohmann::json NavigationPointStore::ListResources() const {
  std::lock_guard<std::mutex> lock(mutex_);
  nlohmann::json result = nlohmann::json::array();
  for (const auto& [robot, points] : robots_)
    for (const auto& [name, point] : points)
      result.push_back({{"class", "navigation_waypoint"},
                        {"uri", "robots/" + robot + "/navigation/waypoints/" + name}});
  return result;
}

std::optional<nlohmann::json> NavigationPointStore::ReadResource(const std::string& uri) const {
  constexpr std::string_view prefix = "robots/";
  constexpr std::string_view middle = "/navigation/waypoints/";
  if (uri.rfind(prefix, 0) != 0) return std::nullopt;
  const std::size_t split = uri.find(middle, prefix.size());
  if (split == std::string::npos) return std::nullopt;
  const std::string robot = uri.substr(prefix.size(), split - prefix.size());
  const std::string name = uri.substr(split + middle.size());
  const NavigationPoint point = Get(robot, name);
  return nlohmann::json{{"reference", "absolute"},
                        {"frame_id", "map"},
                        {"x_m", point.x_m},
                        {"y_m", point.y_m},
                        {"yaw_rad", point.yaw_rad}};
}

bool NavigationPointStore::SupportsResourceTemplate(const std::string& uri_template) const {
  return uri_template == "robots/{robot_name}/navigation/waypoints/{waypoint_id}";
}

}  // namespace eos::runtime::store

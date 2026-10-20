#include "src/eos/runtime/store/furniture_region_store.h"

#include <sys/stat.h>

#include <cerrno>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace eos::runtime::store {
namespace {

constexpr std::size_t kMaximumFileBytes = 1024U * 1024U;
constexpr std::size_t kMaximumRegions = 1000;

void RequireSegment(const std::string& value, const char* label) {
  if (value.empty() || value.size() > 128 || value == "." || value == "..")
    throw std::runtime_error(std::string(label) + " is invalid");
  for (const unsigned char byte : value)
    if (byte == '/' || byte < 0x20 || byte == 0x7f)
      throw std::runtime_error(std::string(label) + " is invalid");
}

double Number(const nlohmann::json& value, const char* key) {
  if (!value.contains(key) || !value.at(key).is_number())
    throw std::runtime_error(std::string("furniture region ") + key + " must be numeric");
  const double result = value.at(key).get<double>();
  if (!std::isfinite(result))
    throw std::runtime_error(std::string("furniture region ") + key + " must be finite");
  return result;
}

}  // namespace

bool FurnitureRegion::Contains(double x_m, double y_m) const {
  return std::isfinite(x_m) && std::isfinite(y_m) && x_m >= min_x_m - margin_m &&
         x_m <= max_x_m + margin_m && y_m >= min_y_m - margin_m &&
         y_m <= max_y_m + margin_m;
}

FurnitureRegionStore::FurnitureRegionStore(std::filesystem::path directory)
    : path_(std::move(directory) / "furniture-regions.v1.json") {
  struct stat status {};
  if (::lstat(path_.c_str(), &status) != 0) {
    if (errno == ENOENT) return;
    throw std::runtime_error("cannot inspect furniture region store '" + path_.string() + "'");
  }
  if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > kMaximumFileBytes)
    throw std::runtime_error("furniture region store is not a bounded regular file");
  std::ifstream input(path_, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read furniture region store '" + path_.string() + "'");
  const std::string encoded{std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>()};
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(encoded);
  } catch (const std::exception& error) {
    throw std::runtime_error("furniture region store contains invalid JSON: " +
                             std::string(error.what()));
  }
  if (!root.is_object() || root.size() != 2 || root.value("format_version", 0) != 1 ||
      !root.contains("robots") || !root.at("robots").is_object())
    throw std::runtime_error("furniture region store envelope is invalid");
  std::size_t count = 0;
  for (const auto& [robot_name, robot] : root.at("robots").items()) {
    RequireSegment(robot_name, "robot name");
    if (!robot.is_object() || robot.size() != 1 || !robot.contains("furniture_regions") ||
        !robot.at("furniture_regions").is_object())
      throw std::runtime_error("robot furniture region table is invalid");
    auto& output = robots_[robot_name];
    for (const auto& [id, value] : robot.at("furniture_regions").items()) {
      RequireSegment(id, "furniture region id");
      if (!value.is_object() || value.size() < 4 || value.size() > 5)
        throw std::runtime_error("furniture region row is invalid");
      for (const auto& [key, ignored] : value.items()) {
        if (key != "min_x_m" && key != "max_x_m" && key != "min_y_m" &&
            key != "max_y_m" && key != "margin_m")
          throw std::runtime_error("furniture region row contains unknown field");
      }
      FurnitureRegion region{id,
                             Number(value, "min_x_m"),
                             Number(value, "max_x_m"),
                             Number(value, "min_y_m"),
                             Number(value, "max_y_m"),
                             value.contains("margin_m") ? Number(value, "margin_m") : 0.3};
      if (region.min_x_m > region.max_x_m || region.min_y_m > region.max_y_m ||
          region.margin_m < 0.0 || region.margin_m > 2.0)
        throw std::runtime_error("furniture region bounds or margin are invalid");
      output.push_back(std::move(region));
      if (++count > kMaximumRegions)
        throw std::runtime_error("furniture region store exceeds 1000 rows");
    }
  }
}

const std::vector<FurnitureRegion>& FurnitureRegionStore::List(
    const std::string& robot_name) const {
  static const std::vector<FurnitureRegion> empty;
  const auto found = robots_.find(robot_name);
  return found == robots_.end() ? empty : found->second;
}

}  // namespace eos::runtime::store

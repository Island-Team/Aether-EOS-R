#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace eos::runtime::store {

struct FurnitureRegion {
  std::string id;
  double min_x_m{0.0};
  double max_x_m{0.0};
  double min_y_m{0.0};
  double max_y_m{0.0};
  double margin_m{0.0};

  bool Contains(double x_m, double y_m) const;
};

/// Read-only deployment-owned map regions loaded from custom-store.
class FurnitureRegionStore final {
 public:
  explicit FurnitureRegionStore(std::filesystem::path directory);

  const std::vector<FurnitureRegion>& List(const std::string& robot_name) const;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
  std::map<std::string, std::vector<FurnitureRegion>> robots_;
};

}  // namespace eos::runtime::store

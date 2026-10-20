#include <cassert>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "src/eos/ext/simulator/map_store.h"

namespace {
class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-simulator-storage-test-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

nlohmann::json Manifest(const std::string& id) {
  return {{"id", id},
          {"version", 2},
          {"revision", 0},
          {"raster",
           {{"path", "raster.png"},
            {"width_px", 4},
            {"height_px", 3},
            {"meters_per_pixel", 0.1},
            {"origin", {{"x_m", -0.2}, {"y_m", -0.15}}}}},
          {"initial_base_pose", {{"x_m", 0.0}, {"y_m", 0.0}, {"yaw_rad", 0.0}}},
          {"lines", nlohmann::json::array()}};
}
}  // namespace

int main() {
  TemporaryDirectory temporary;
  eos::ext::simulator::SourceLibrary library(temporary.path);
  eos::ext::simulator::MapStore maps(temporary.path);

  const std::vector<std::uint8_t> media{1, 2, 3};
  auto source = library.Write("screen", "image/png", media, 0);
  assert(source.at("path") == "files/screen.png");
  assert(library.List().at("revision") == 1);
  assert(library.Read("screen").second == media);
  assert(library.Resolve("screen").parent_path() == library.root() / "files");

  auto created = maps.Create(Manifest("lab"));
  assert(created.at("version") == 2);
  assert(std::filesystem::is_regular_file(temporary.path / "simulator/maps/lab/manifest.json"));
  assert(std::filesystem::is_regular_file(temporary.path / "simulator/maps/lab/raster.png"));
  assert(!std::filesystem::exists(temporary.path / "demo/maps"));

  created["lines"] = nlohmann::json::array({{{"id", "display"},
                                             {"source_id", "screen"},
                                             {"center", {{"x_m", 1.0}, {"y_m", 0.0}}},
                                             {"yaw_rad", 1.5707963267948966},
                                             {"length_m", 1.0}}});
  const auto saved = maps.ReplaceMetadata("lab", created, 0);
  assert(saved.at("revision") == 1);

  bool conflict = false;
  try {
    (void)maps.ReplaceMetadata("lab", saved, 0);
  } catch (const std::domain_error&) {
    conflict = true;
  }
  assert(conflict);

  bool referenced = false;
  try {
    library.Remove("screen", 1);
  } catch (const std::invalid_argument&) {
    referenced = true;
  }
  assert(referenced);

  auto invalid = saved;
  invalid["lines"][0]["source_id"] = "missing";
  bool missing = false;
  try {
    (void)maps.ReplaceMetadata("lab", invalid, 1);
  } catch (const std::invalid_argument&) {
    missing = true;
  }
  assert(missing);

  maps.Remove("lab", 1);
  library.Remove("screen", 1);
  assert(library.List().at("sources").empty());
}

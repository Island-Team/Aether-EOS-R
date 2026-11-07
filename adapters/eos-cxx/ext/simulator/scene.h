/// Validated visual-source scenes and planar camera-ray selection.
#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
namespace eos::ext::simulator {
struct Point2 {
  double x_m{0.0};
  double y_m{0.0};
};
struct SourceLine {
  std::string id;
  std::string source_id;
  Point2 center;
  double yaw_rad{0.0};
  double length_m{0.0};
};
struct Scene {
  std::uint64_t revision{0};
  std::vector<SourceLine> lines;
};
struct LineHit {
  std::string line_id;
  std::string source_id;
  Point2 point;
  double distance_m{0.0};
};
/// Parses the strict v2 geometry schema. MapStore separately validates source
/// references against the process-wide SourceLibrary.
Scene ParseScene(const nlohmann::json& value);
nlohmann::json SceneJson(const Scene& scene);
std::optional<LineHit> SelectSourceLine(const Scene& scene, Point2 origin, double ray_angle_rad,
                                        double max_range_m);
}  // namespace eos::ext::simulator

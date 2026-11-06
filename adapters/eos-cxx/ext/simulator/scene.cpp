#include "scene.h"

#include <cmath>
#include <regex>
#include <stdexcept>
#include <unordered_set>
namespace eos::ext::simulator {
namespace {
using Json = nlohmann::json;
void RequireKeys(const Json& value, std::initializer_list<const char*> keys, const char* context) {
  if (!value.is_object()) throw std::invalid_argument(std::string(context) + " must be an object");
  for (auto item = value.begin(); item != value.end(); ++item) {
    bool admitted = false;
    for (const char* key : keys) admitted = admitted || item.key() == key;
    if (!admitted)
      throw std::invalid_argument(std::string(context) + " has unknown key: " + item.key());
  }
}
void ValidateId(const std::string& id, const char* context) {
  static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$");
  if (!std::regex_match(id, pattern))
    throw std::invalid_argument(std::string(context) + " is invalid");
}
double Finite(const Json& value, const char* key, const char* context) {
  const double number = value.at(key).get<double>();
  if (!std::isfinite(number))
    throw std::invalid_argument(std::string(context) + "." + key + " must be finite");
  return number;
}
double Cross(Point2 a, Point2 b) { return a.x_m * b.y_m - a.y_m * b.x_m; }
Point2 Minus(Point2 a, Point2 b) { return {a.x_m - b.x_m, a.y_m - b.y_m}; }
}  // namespace
Scene ParseScene(const Json& value) {
  RequireKeys(value, {"version", "revision", "lines"}, "scene");
  if (value.at("version").get<std::uint32_t>() != 2)
    throw std::invalid_argument("unsupported scene version");
  if (!value.at("lines").is_array()) throw std::invalid_argument("scene lines must be an array");
  Scene scene{.revision = value.at("revision").get<std::uint64_t>()};
  std::unordered_set<std::string> line_ids;
  for (const auto& value_line : value.at("lines")) {
    RequireKeys(value_line, {"id", "source_id", "center", "yaw_rad", "length_m"}, "scene line");
    const auto& center = value_line.at("center");
    RequireKeys(center, {"x_m", "y_m"}, "scene line center");
    SourceLine line{.id = value_line.at("id").get<std::string>(),
                    .source_id = value_line.at("source_id").get<std::string>(),
                    .center = {Finite(center, "x_m", "scene line center"),
                               Finite(center, "y_m", "scene line center")},
                    .yaw_rad = Finite(value_line, "yaw_rad", "scene line"),
                    .length_m = Finite(value_line, "length_m", "scene line")};
    ValidateId(line.id, "line id");
    ValidateId(line.source_id, "line source_id");
    if (!line_ids.insert(line.id).second) throw std::invalid_argument("duplicate line id");
    if (line.length_m <= 0.0) throw std::invalid_argument("line length_m must be positive");
    scene.lines.push_back(std::move(line));
  }
  return scene;
}
Json SceneJson(const Scene& scene) {
  Json lines = Json::array();
  for (const auto& line : scene.lines)
    lines.push_back({{"id", line.id},
                     {"source_id", line.source_id},
                     {"center", {{"x_m", line.center.x_m}, {"y_m", line.center.y_m}}},
                     {"yaw_rad", line.yaw_rad},
                     {"length_m", line.length_m}});
  return {{"version", 2}, {"revision", scene.revision}, {"lines", lines}};
}
std::optional<LineHit> SelectSourceLine(const Scene& scene, Point2 origin, double ray_angle_rad,
                                        double max_range_m) {
  if (!std::isfinite(origin.x_m) || !std::isfinite(origin.y_m) || !std::isfinite(ray_angle_rad) ||
      !std::isfinite(max_range_m) || max_range_m <= 0.0)
    throw std::invalid_argument("camera ray and range must be finite with positive range");
  const Point2 ray{std::cos(ray_angle_rad), std::sin(ray_angle_rad)};
  std::optional<LineHit> nearest;
  for (const auto& line : scene.lines) {
    const Point2 axis{std::cos(line.yaw_rad), std::sin(line.yaw_rad)};
    const Point2 start{line.center.x_m - axis.x_m * line.length_m * 0.5,
                       line.center.y_m - axis.y_m * line.length_m * 0.5};
    const Point2 segment{axis.x_m * line.length_m, axis.y_m * line.length_m};
    const double denominator = Cross(ray, segment);
    if (std::abs(denominator) <= 1e-12) continue;
    const Point2 delta = Minus(start, origin);
    const double distance = Cross(delta, segment) / denominator;
    const double along = Cross(delta, ray) / denominator;
    if (distance <= 0.0 || distance > max_range_m || along < 0.0 || along > 1.0) continue;
    if (!nearest || distance < nearest->distance_m)
      nearest = LineHit{line.id,
                        line.source_id,
                        {origin.x_m + ray.x_m * distance, origin.y_m + ray.y_m * distance},
                        distance};
  }
  return nearest;
}
}  // namespace eos::ext::simulator

#include "raster_map.h"

#include <png.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace eos::ext::simulator {
namespace {

constexpr double kPi = 3.14159265358979323846;

double Normalize(double value) {
  while (value > kPi) value -= 2.0 * kPi;
  while (value < -kPi) value += 2.0 * kPi;
  return value;
}

void RequireFinite(double value, const char* name) {
  if (!std::isfinite(value)) throw std::invalid_argument(std::string(name) + " must be finite");
}

RasterMapMetadata LoadMetadata(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open raster map metadata: " + path.string() +
                             "; create the map through the simulator API");
  }

  nlohmann::json json;
  try {
    input >> json;
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument("invalid raster map metadata JSON: " + std::string(error.what()));
  }

  RasterMapMetadata result;
  try {
    result.id = json.at("id").get<std::string>();
    result.version = json.at("version").get<std::uint32_t>();
    result.revision = json.at("revision").get<std::uint64_t>();
    const auto& raster = json.at("raster");
    result.width_px = raster.at("width_px").get<std::uint32_t>();
    result.height_px = raster.at("height_px").get<std::uint32_t>();
    result.meters_per_pixel = raster.at("meters_per_pixel").get<double>();
    const auto& origin = raster.at("origin");
    result.origin_x_m = origin.at("x_m").get<double>();
    result.origin_y_m = origin.at("y_m").get<double>();
    const auto& pose = json.at("initial_base_pose");
    result.initial_base_pose.x_m = pose.at("x_m").get<double>();
    result.initial_base_pose.y_m = pose.at("y_m").get<double>();
    result.initial_base_pose.yaw_rad = pose.at("yaw_rad").get<double>();
  } catch (const nlohmann::json::exception& error) {
    throw std::invalid_argument("invalid raster map metadata fields: " + std::string(error.what()));
  }

  if (result.version != 2) throw std::invalid_argument("unsupported raster map version");
  if (result.width_px == 0 || result.height_px == 0)
    throw std::invalid_argument("raster map dimensions must be non-zero");
  RequireFinite(result.meters_per_pixel, "meters_per_pixel");
  RequireFinite(result.origin_x_m, "origin.x_m");
  RequireFinite(result.origin_y_m, "origin.y_m");
  RequireFinite(result.initial_base_pose.x_m, "initial_base_pose.x_m");
  RequireFinite(result.initial_base_pose.y_m, "initial_base_pose.y_m");
  RequireFinite(result.initial_base_pose.yaw_rad, "initial_base_pose.yaw_rad");
  if (result.meters_per_pixel <= 0.0)
    throw std::invalid_argument("meters_per_pixel must be positive");
  if (static_cast<std::uint64_t>(result.width_px) * result.height_px >
      std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("raster map is too large");
  return result;
}

std::vector<RasterCell> LoadPng(const std::filesystem::path& path,
                                const RasterMapMetadata& metadata) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  const std::string filename = path.string();
  if (!png_image_begin_read_from_file(&image, filename.c_str()))
    throw std::runtime_error("cannot read raster map PNG: " + filename);
  if ((image.format & PNG_FORMAT_FLAG_ALPHA) != 0) {
    png_image_free(&image);
    throw std::invalid_argument("raster PNG must not contain an alpha channel");
  }
  if (image.width != metadata.width_px || image.height != metadata.height_px) {
    png_image_free(&image);
    throw std::invalid_argument("raster PNG dimensions do not match metadata");
  }
  image.format = PNG_FORMAT_RGB;
  std::vector<png_byte> pixels(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
    const std::string message = image.message;
    png_image_free(&image);
    throw std::runtime_error("cannot decode raster map PNG: " + message);
  }
  png_image_free(&image);

  std::vector<RasterCell> cells;
  cells.reserve(static_cast<std::size_t>(metadata.width_px) * metadata.height_px);
  for (std::size_t i = 0; i < pixels.size(); i += 3) {
    const auto r = pixels[i], g = pixels[i + 1], b = pixels[i + 2];
    if (r != g || r != b || (r != 0 && r != 127 && r != 255))
      throw std::invalid_argument("raster PNG contains a non-canonical pixel");
    cells.push_back(static_cast<RasterCell>(r));
  }
  return cells;
}

struct Point {
  double x;
  double y;
};

std::array<Point, 4> Footprint(const PlanarPose& pose, double width, double length) {
  const double c = std::cos(pose.yaw_rad), s = std::sin(pose.yaw_rad);
  const double hx = length / 2.0, hy = width / 2.0;
  std::array<Point, 4> points{{{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}}};
  for (auto& point : points) {
    const double x = point.x;
    point.x = pose.x_m + c * x - s * point.y;
    point.y = pose.y_m + s * x + c * point.y;
  }
  return points;
}

bool OverlapsCell(const std::array<Point, 4>& rectangle, double left, double bottom, double size) {
  const std::array<Point, 4> cell{
      {{left, bottom}, {left + size, bottom}, {left + size, bottom + size}, {left, bottom + size}}};
  const std::array<Point, 4> axes{
      {{1.0, 0.0},
       {0.0, 1.0},
       {rectangle[1].x - rectangle[0].x, rectangle[1].y - rectangle[0].y},
       {rectangle[3].x - rectangle[0].x, rectangle[3].y - rectangle[0].y}}};
  for (const auto& axis : axes) {
    double r_min = std::numeric_limits<double>::infinity(), r_max = -r_min;
    double c_min = r_min, c_max = r_max;
    for (const auto& point : rectangle) {
      const double value = point.x * axis.x + point.y * axis.y;
      r_min = std::min(r_min, value);
      r_max = std::max(r_max, value);
    }
    for (const auto& point : cell) {
      const double value = point.x * axis.x + point.y * axis.y;
      c_min = std::min(c_min, value);
      c_max = std::max(c_max, value);
    }
    if (r_max < c_min || c_max < r_min) return false;
  }
  return true;
}

void ValidateFootprint(const PlanarPose& pose, double width, double length) {
  RequireFinite(pose.x_m, "pose.x_m");
  RequireFinite(pose.y_m, "pose.y_m");
  RequireFinite(pose.yaw_rad, "pose.yaw_rad");
  RequireFinite(width, "footprint width");
  RequireFinite(length, "footprint length");
  if (width <= 0.0 || length <= 0.0)
    throw std::invalid_argument("footprint dimensions must be positive");
}

}  // namespace

RasterMap RasterMap::LoadBundle(const std::filesystem::path& bundle_path) {
  auto metadata = LoadMetadata(bundle_path / "manifest.json");
  auto cells = LoadPng(bundle_path / "raster.png", metadata);
  return RasterMap(metadata, std::move(cells));
}

RasterMap RasterMap::EmptyWhite(std::uint32_t width_px, std::uint32_t height_px,
                                double meters_per_pixel) {
  if (width_px == 0 || height_px == 0)
    throw std::invalid_argument("raster map dimensions must be non-zero");
  RequireFinite(meters_per_pixel, "meters_per_pixel");
  if (meters_per_pixel <= 0.0) throw std::invalid_argument("meters_per_pixel must be positive");
  if (static_cast<std::uint64_t>(width_px) * height_px > std::numeric_limits<std::uint32_t>::max())
    throw std::invalid_argument("raster map is too large");
  RasterMapMetadata metadata;
  metadata.width_px = width_px;
  metadata.height_px = height_px;
  metadata.meters_per_pixel = meters_per_pixel;
  metadata.origin_x_m = -static_cast<double>(width_px) * meters_per_pixel / 2.0;
  metadata.origin_y_m = -static_cast<double>(height_px) * meters_per_pixel / 2.0;
  return RasterMap(metadata, std::vector<RasterCell>(static_cast<std::size_t>(width_px) * height_px,
                                                     RasterCell::kFree));
}

std::vector<std::uint8_t> RasterMap::EncodePng() const {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = metadata_.width_px;
  image.height = metadata_.height_px;
  image.format = PNG_FORMAT_GRAY;
  std::vector<png_byte> pixels;
  pixels.reserve(cells_.size());
  for (const RasterCell cell : cells_) pixels.push_back(static_cast<png_byte>(cell));
  png_alloc_size_t encoded_size = 0;
  if (!png_image_write_to_memory(&image, nullptr, &encoded_size, 0, pixels.data(), 0, nullptr))
    throw std::runtime_error("cannot size canonical raster PNG");
  std::vector<std::uint8_t> encoded(encoded_size);
  if (!png_image_write_to_memory(&image, encoded.data(), &encoded_size, 0, pixels.data(), 0,
                                 nullptr))
    throw std::runtime_error("cannot encode canonical raster PNG");
  encoded.resize(encoded_size);
  return encoded;
}

RasterCell RasterMap::cell(std::uint32_t column, std::uint32_t row) const {
  if (column >= metadata_.width_px || row >= metadata_.height_px)
    throw std::out_of_range("raster cell is outside the map");
  return cells_[static_cast<std::size_t>(row) * metadata_.width_px + column];
}

double RasterMap::PxToMeter(double px) const { return px * metadata_.meters_per_pixel; }

std::optional<RasterCollision> RasterMap::CheckFootprint(const PlanarPose& pose, double width_m,
                                                         double length_m) const {
  ValidateFootprint(pose, width_m, length_m);
  const auto rectangle = Footprint(pose, width_m, length_m);
  const double right = metadata_.origin_x_m + PxToMeter(metadata_.width_px);
  const double top = metadata_.origin_y_m + PxToMeter(metadata_.height_px);
  for (const auto& point : rectangle) {
    if (point.x < metadata_.origin_x_m || point.x > right || point.y < metadata_.origin_y_m ||
        point.y > top)
      return RasterCollision{CollisionKind::kOutOfBounds, -1, -1, pose};
  }

  const auto [min_x, max_x] = std::minmax_element(
      rectangle.begin(), rectangle.end(), [](const Point& a, const Point& b) { return a.x < b.x; });
  const auto [min_y, max_y] = std::minmax_element(
      rectangle.begin(), rectangle.end(), [](const Point& a, const Point& b) { return a.y < b.y; });
  const auto clamp_column = [&](double x) {
    return std::clamp(
        static_cast<int>(std::floor((x - metadata_.origin_x_m) / metadata_.meters_per_pixel)), 0,
        static_cast<int>(metadata_.width_px) - 1);
  };
  const auto bottom_row = [&](double y) {
    return std::clamp(
        static_cast<int>(std::floor((y - metadata_.origin_y_m) / metadata_.meters_per_pixel)), 0,
        static_cast<int>(metadata_.height_px) - 1);
  };
  const int first_column = clamp_column(min_x->x), last_column = clamp_column(max_x->x);
  const int first_bottom_row = bottom_row(min_y->y), last_bottom_row = bottom_row(max_y->y);
  for (int bottom = first_bottom_row; bottom <= last_bottom_row; ++bottom) {
    const int image_row = static_cast<int>(metadata_.height_px) - 1 - bottom;
    for (int column = first_column; column <= last_column; ++column) {
      const RasterCell value = cell(column, image_row);
      if (value == RasterCell::kFree) continue;
      const double left = metadata_.origin_x_m + PxToMeter(column);
      const double cell_bottom = metadata_.origin_y_m + PxToMeter(bottom);
      if (!OverlapsCell(rectangle, left, cell_bottom, PxToMeter(1.0))) continue;
      return RasterCollision{
          value == RasterCell::kUnknown ? CollisionKind::kUnknown : CollisionKind::kOccupied,
          column, image_row, pose};
    }
  }
  return std::nullopt;
}

std::optional<RasterCollision> RasterMap::CheckSweptFootprint(const PlanarPose& start,
                                                              const PlanarPose& end, double width_m,
                                                              double length_m) const {
  ValidateFootprint(start, width_m, length_m);
  ValidateFootprint(end, width_m, length_m);
  const double dx = end.x_m - start.x_m, dy = end.y_m - start.y_m;
  const double dyaw = Normalize(end.yaw_rad - start.yaw_rad);
  const double radius = std::hypot(width_m, length_m) / 2.0;
  const double maximum_point_travel = std::hypot(dx, dy) + std::abs(dyaw) * radius;
  const auto steps = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(std::ceil(maximum_point_travel / PxToMeter(0.5))));
  for (std::uint64_t step = 0; step <= steps; ++step) {
    const double t = static_cast<double>(step) / static_cast<double>(steps);
    PlanarPose pose{start.x_m + dx * t, start.y_m + dy * t, Normalize(start.yaw_rad + dyaw * t)};
    if (auto collision = CheckFootprint(pose, width_m, length_m)) return collision;
  }
  return std::nullopt;
}

}  // namespace eos::ext::simulator

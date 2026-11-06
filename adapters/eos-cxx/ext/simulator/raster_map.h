/// Raster occupancy map and planar base collision checks for the EOS simulator.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <vector>

namespace eos::ext::simulator {

enum class RasterCell : std::uint8_t { kFree = 255, kUnknown = 127, kOccupied = 0 };

struct PlanarPose {
  double x_m{0.0};
  double y_m{0.0};
  double yaw_rad{0.0};
};

struct RasterMapMetadata {
  std::string id{"fallback"};
  std::uint32_t version{1};
  std::uint64_t revision{0};
  std::uint32_t width_px{0};
  std::uint32_t height_px{0};
  double meters_per_pixel{0.0};
  double origin_x_m{0.0};
  double origin_y_m{0.0};
  PlanarPose initial_base_pose;
};

enum class CollisionKind { kUnknown, kOccupied, kOutOfBounds };

struct RasterCollision {
  CollisionKind kind{CollisionKind::kOutOfBounds};
  /// Image coordinates (origin at the top-left). Set to -1 for out-of-bounds.
  int column{-1};
  int row{-1};
  PlanarPose pose;
};

/// Immutable canonical grayscale occupancy map loaded from a map bundle.
class RasterMap {
 public:
  /// Loads `<bundle_path>/map_<id>.json` and its bound `map_<id>.png`.
  /// Throws std::runtime_error when files cannot be decoded and
  /// std::invalid_argument when their contents violate the map format.
  static RasterMap LoadBundle(const std::filesystem::path& bundle_path);

  /// Creates a finite all-free map for simulator startup without a bundle.
  static RasterMap EmptyWhite(std::uint32_t width_px = 100, std::uint32_t height_px = 100,
                              double meters_per_pixel = 0.1);

  const RasterMapMetadata& metadata() const { return metadata_; }
  RasterCell cell(std::uint32_t column, std::uint32_t row) const;

  /// Encodes the validated cells as a canonical 8-bit grayscale PNG.
  std::vector<std::uint8_t> EncodePng() const;

  /// Converts a distance in pixels to metres using the map resolution.
  double PxToMeter(double px) const;

  /// Checks an oriented rectangular footprint at one world-frame pose.
  std::optional<RasterCollision> CheckFootprint(const PlanarPose& pose, double width_m,
                                                double length_m) const;

  /// Checks a straight-line translation with shortest-path yaw interpolation.
  /// Samples densely enough that no footprint point advances by more than half
  /// a cell between samples. The start and end poses are both checked.
  std::optional<RasterCollision> CheckSweptFootprint(const PlanarPose& start, const PlanarPose& end,
                                                     double width_m, double length_m) const;

 private:
  RasterMap(RasterMapMetadata metadata, std::vector<RasterCell> cells)
      : metadata_(metadata), cells_(std::move(cells)) {}

  RasterMapMetadata metadata_;
  std::vector<RasterCell> cells_;
};

}  // namespace eos::ext::simulator

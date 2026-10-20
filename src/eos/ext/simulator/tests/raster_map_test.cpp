#include "src/eos/ext/simulator/raster_map.h"

#include <png.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using eos::ext::simulator::CollisionKind;
using eos::ext::simulator::PlanarPose;
using eos::ext::simulator::RasterCell;
using eos::ext::simulator::RasterMap;

class TemporaryBundle {
 public:
  TemporaryBundle()
      : path(std::filesystem::temp_directory_path() /
             ("eos-raster-test-" +
              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
    std::filesystem::create_directories(path);
  }
  ~TemporaryBundle() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
};

void WriteMetadata(const std::filesystem::path& path, unsigned width = 6, unsigned height = 6) {
  const std::string id = path.filename().string();
  std::ofstream(path / "manifest.json")
      << R"({"id":")" << id << R"(","version":2,"revision":0,"raster":{"path":"raster.png","width_px":)"
      << width << R"(,"height_px":)" << height
      << R"(,"meters_per_pixel":1.0,"origin":{"x_m":-1.0,"y_m":-2.0}},)"
         R"("initial_base_pose":{"x_m":0.0,"y_m":0.0,"yaw_rad":0.0},"lines":[]})";
}

void WritePng(const std::filesystem::path& path, unsigned width, unsigned height,
              const std::vector<unsigned char>& rgb) {
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  image.width = width;
  image.height = height;
  image.format = PNG_FORMAT_RGB;
  assert(png_image_write_to_file(&image, (path / "raster.png").c_str(), 0, rgb.data(), 0,
                                 nullptr));
}

std::vector<unsigned char> White(unsigned width = 6, unsigned height = 6) {
  return std::vector<unsigned char>(width * height * 3, 255);
}

void SetPixel(std::vector<unsigned char>* pixels, unsigned width, unsigned column, unsigned row,
              unsigned char value) {
  const std::size_t offset = (row * width + column) * 3;
  (*pixels)[offset] = (*pixels)[offset + 1] = (*pixels)[offset + 2] = value;
}

template <typename Function>
bool ThrowsInvalid(Function function) {
  try {
    function();
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}

template <typename Function>
std::string RuntimeError(Function function) {
  try {
    function();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

}  // namespace

int main() {
  {
    const RasterMap map = RasterMap::EmptyWhite();
    assert(map.metadata().width_px == 100);
    assert(map.metadata().height_px == 100);
    assert(map.cell(0, 0) == RasterCell::kFree);
    assert(!map.CheckFootprint({0.0, 0.0, 0.0}, 0.3, 0.5));
  }

  {
    TemporaryBundle bundle;
    const std::string error = RuntimeError([&] { RasterMap::LoadBundle(bundle.path); });
    assert(error.find("cannot open raster map metadata") != std::string::npos);
  }

  {
    TemporaryBundle bundle;
    WriteMetadata(bundle.path);
    auto pixels = White();
    SetPixel(&pixels, 6, 2, 4, 0);    // world cell x=[1,2], y=[-1,0]
    SetPixel(&pixels, 6, 4, 1, 127);  // world cell x=[3,4], y=[2,3]
    WritePng(bundle.path, 6, 6, pixels);
    const RasterMap map = RasterMap::LoadBundle(bundle.path);
    assert(map.metadata().origin_x_m == -1.0);
    assert(map.PxToMeter(2.5) == 2.5);
    assert(map.cell(2, 4) == RasterCell::kOccupied);
    assert(map.cell(4, 1) == RasterCell::kUnknown);

    assert(!map.CheckFootprint({0.0, 0.5, 0.0}, 0.5, 0.5));
    auto occupied = map.CheckFootprint({1.5, -0.5, 0.0}, 0.5, 0.5);
    assert(occupied && occupied->kind == CollisionKind::kOccupied);
    assert(occupied->column == 2 && occupied->row == 4);
    auto unknown = map.CheckFootprint({3.5, 2.5, 0.4}, 0.5, 0.5);
    assert(unknown && unknown->kind == CollisionKind::kUnknown);
    auto outside = map.CheckFootprint({-0.9, -1.9, 0.0}, 0.5, 0.5);
    assert(outside && outside->kind == CollisionKind::kOutOfBounds);

    // The destination is clear, but the straight-line sweep crosses the black cell.
    auto swept = map.CheckSweptFootprint({0.0, -0.5, 0.0}, {3.0, -0.5, 0.0}, 0.4, 0.4);
    assert(swept && swept->kind == CollisionKind::kOccupied);
    // Rotation-only motion is sampled and remains valid in open space.
    assert(!map.CheckSweptFootprint({0.0, 1.0, 0.0}, {0.0, 1.0, 3.0}, 0.4, 1.2));
  }

  {
    TemporaryBundle bundle;
    WriteMetadata(bundle.path, 2, 2);
    auto pixels = White(2, 2);
    SetPixel(&pixels, 2, 0, 0, 128);
    WritePng(bundle.path, 2, 2, pixels);
    assert(ThrowsInvalid([&] { RasterMap::LoadBundle(bundle.path); }));
  }

  {
    TemporaryBundle bundle;
    WriteMetadata(bundle.path, 3, 2);
    WritePng(bundle.path, 2, 2, White(2, 2));
    assert(ThrowsInvalid([&] { RasterMap::LoadBundle(bundle.path); }));
  }
}

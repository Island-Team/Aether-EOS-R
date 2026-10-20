#include "src/eos/runtime/store/furniture_region_store.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-furniture-store-" + std::to_string(::getpid()) + "-" +
              std::to_string(++sequence))) {
    std::filesystem::create_directories(path);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
  static inline int sequence{0};
};

TEST(FurnitureRegionStoreTest, LoadsRobotScopedRectanglesWithDefaultMargin) {
  TemporaryDirectory directory;
  std::ofstream output(directory.path / "furniture-regions.v1.json");
  output << R"({"format_version":1,"robots":{"demo-0":{"furniture_regions":{"sofa":{"min_x_m":1,"max_x_m":2,"min_y_m":3,"max_y_m":4}}}}})";
  output.close();

  eos::runtime::store::FurnitureRegionStore store(directory.path);
  ASSERT_EQ(store.List("demo-0").size(), 1U);
  const auto& region = store.List("demo-0").front();
  EXPECT_EQ(region.id, "sofa");
  EXPECT_TRUE(region.Contains(0.7, 4.3));
  EXPECT_FALSE(region.Contains(0.69, 4.0));
  EXPECT_TRUE(store.List("other").empty());
}

TEST(FurnitureRegionStoreTest, MissingFileMeansNoConfiguredRegions) {
  TemporaryDirectory directory;
  eos::runtime::store::FurnitureRegionStore store(directory.path);
  EXPECT_TRUE(store.List("demo-0").empty());
}

TEST(FurnitureRegionStoreTest, RejectsUnknownFieldsAndInvalidBounds) {
  TemporaryDirectory directory;
  const auto write = [&](const char* row) {
    std::ofstream output(directory.path / "furniture-regions.v1.json");
    output << "{\"format_version\":1,\"robots\":{\"r\":{\"furniture_regions\":{\"x\":"
           << row << "}}}}";
  };
  write(R"({"min_x_m":2,"max_x_m":1,"min_y_m":0,"max_y_m":1})");
  EXPECT_THROW(eos::runtime::store::FurnitureRegionStore(directory.path), std::runtime_error);
  write(R"({"min_x_m":0,"max_x_m":1,"min_y_m":0,"max_y_m":1,"extra":1})");
  EXPECT_THROW(eos::runtime::store::FurnitureRegionStore(directory.path), std::runtime_error);
}

}  // namespace

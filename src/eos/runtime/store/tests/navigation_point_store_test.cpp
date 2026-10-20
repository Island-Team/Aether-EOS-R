#include "src/eos/runtime/store/navigation_point_store.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path(std::filesystem::temp_directory_path() /
             ("eos-navigation-store-" + std::to_string(::getpid()) + "-" +
              std::to_string(++sequence))) {}
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
  std::filesystem::path path;
  static inline int sequence{0};
};

TEST(NavigationPointStoreTest, PersistsRobotScopedPointsAndProjectsAbsoluteResource) {
  TemporaryDirectory directory;
  {
    eos::runtime::store::NavigationPointStore store(directory.path);
    store.Put("demo-0", {"exit", 1.0, 2.0, 0.5});
    store.Put("other", {"exit", 1.0, 2.0, 3.0});
    EXPECT_EQ(store.List("demo-0").size(), 1U);
    const auto resource = store.ReadResource("robots/demo-0/navigation/waypoints/exit");
    ASSERT_TRUE(resource);
    EXPECT_EQ(resource->at("reference"), "absolute");
    EXPECT_EQ(resource->at("frame_id"), "map");
    EXPECT_DOUBLE_EQ(resource->at("x_m"), 1.0);
  }
  eos::runtime::store::NavigationPointStore restored(directory.path);
  EXPECT_DOUBLE_EQ(restored.Get("other", "exit").yaw_rad, 3.0);
}

TEST(NavigationPointStoreTest, DeleteAndReloadAreAtomicFromTheReadersPerspective) {
  TemporaryDirectory directory;
  eos::runtime::store::NavigationPointStore store(directory.path);
  store.Put("demo-0", {"home", 1.0, 2.0, 3.0});
  store.Delete("demo-0", "home");
  EXPECT_THROW(store.Get("demo-0", "home"), eos::runtime::store::StoreError);

  std::ofstream output(store.path());
  output
      << R"({"format_version":1,"robots":{"demo-0":{"navigation_points":{"new":{"x_m":4,"y_m":5,"yaw_rad":6}}}}})";
  output.close();
  EXPECT_EQ(store.Reload(), 1U);
  EXPECT_DOUBLE_EQ(store.Get("demo-0", "new").x_m, 4.0);
}

TEST(NavigationPointStoreTest, ExternalEditBlocksWritesUntilReload) {
  TemporaryDirectory directory;
  eos::runtime::store::NavigationPointStore store(directory.path);
  store.Put("demo-0", {"home", 1.0, 2.0, 3.0});
  std::ofstream output(store.path(), std::ios::app);
  output << " ";
  output.close();
  try {
    store.Put("demo-0", {"other", 4.0, 5.0, 6.0});
    FAIL() << "expected external edit conflict";
  } catch (const eos::runtime::store::StoreError& error) {
    EXPECT_EQ(error.code(), eos::runtime::store::StoreError::Code::kConflict);
  }
  EXPECT_EQ(store.Reload(), 1U);
  EXPECT_NO_THROW(store.Put("demo-0", {"other", 4.0, 5.0, 6.0}));
}

TEST(NavigationPointStoreTest, RejectsUnknownFieldsNonfiniteValuesAndSymlinks) {
  TemporaryDirectory directory;
  std::filesystem::create_directories(directory.path);
  {
    std::ofstream output(directory.path / "navigation-points.v1.json");
    output
        << R"({"format_version":1,"robots":{"r":{"navigation_points":{"p":{"x_m":1,"y_m":2,"yaw_rad":3,"z_m":4}}}}})";
  }
  EXPECT_THROW(eos::runtime::store::NavigationPointStore(directory.path),
               eos::runtime::store::StoreError);
  std::filesystem::remove(directory.path / "navigation-points.v1.json");
  std::filesystem::create_symlink("missing", directory.path / "navigation-points.v1.json");
  EXPECT_THROW(eos::runtime::store::NavigationPointStore(directory.path),
               eos::runtime::store::StoreError);
}

TEST(NavigationPointStoreTest, RejectsMalformedVersionWithTypedError) {
  TemporaryDirectory directory;
  std::filesystem::create_directories(directory.path);
  std::ofstream output(directory.path / "navigation-points.v1.json");
  output << R"({"format_version":"one","robots":{}})";
  output.close();
  try {
    eos::runtime::store::NavigationPointStore store(directory.path);
    FAIL() << "expected invalid store error";
  } catch (const eos::runtime::store::StoreError& error) {
    EXPECT_EQ(error.code(), eos::runtime::store::StoreError::Code::kInvalid);
  }
}

}  // namespace

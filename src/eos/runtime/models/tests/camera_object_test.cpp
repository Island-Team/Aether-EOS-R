#include "src/eos/runtime/models/camera_object.h"

#include <gtest/gtest.h>

namespace eos::runtime::models {
namespace {

TEST(CameraObjectStoreTest, PublishesIndependentMonotonicSnapshots) {
  CameraObjectStore store;
  unsigned updates = 0;
  store.SetUpdatedCallback([&](const std::string& device_id) {
    ++updates;
    EXPECT_TRUE(store.Latest().has_value());
    EXPECT_EQ(device_id, "cam-1");
  });
  CameraObjectStore::Snapshot first;
  first.model_id = "yolo11-litter";
  first.device_id = "cam-1";
  first.part_id = "head";
  first.generation = 4;
  first.jpeg = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0xff, 0xd8, 0xff, 0xd9});
  first.response = {{"objects", nlohmann::json::array()}, {"object_count", 0}};
  store.Set(std::move(first));

  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->sequence, 1U);
  EXPECT_EQ(store.Latest()->generation, 4U);
  EXPECT_EQ(store.Latest()->device_id, "cam-1");
  EXPECT_EQ(store.Latest()->part_id, "head");
  ASSERT_TRUE(store.Latest()->jpeg);
  EXPECT_EQ(*store.Latest()->jpeg,
            (std::vector<std::uint8_t>{0xff, 0xd8, 0xff, 0xd9}));
  EXPECT_EQ(updates, 1U);

  CameraObjectStore::Snapshot second;
  second.model_id = "yolo11-litter";
  second.device_id = "cam-1";
  second.part_id = "head";
  second.generation = 5;
  second.response = {{"objects", {{{"label", "bottle"}, {"confidence", 0.9}}}},
                     {"object_count", 1}};
  store.Set(std::move(second));
  ASSERT_TRUE(store.Latest().has_value());
  EXPECT_EQ(store.Latest()->sequence, 2U);
  EXPECT_EQ(store.Latest()->response.at("object_count"), 1);
  EXPECT_EQ(updates, 2U);
}

TEST(CameraObjectStoreTest, RetainsLatestSnapshotPerCamera) {
  CameraObjectStore store;
  CameraObjectStore::Snapshot wrist;
  wrist.device_id = "cam-0";
  wrist.part_id = "default";
  store.Set(std::move(wrist));
  CameraObjectStore::Snapshot head;
  head.device_id = "cam-1";
  head.part_id = "head";
  store.Set(std::move(head));
  ASSERT_TRUE(store.Latest("cam-0"));
  ASSERT_TRUE(store.Latest("cam-1"));
  EXPECT_EQ(store.Latest("cam-0")->sequence, 1U);
  EXPECT_EQ(store.Latest("cam-1")->sequence, 2U);
  EXPECT_FALSE(store.Latest("cam-2"));
}

TEST(CameraObjectStoreTest, RejectsStaleAndFutureFrames) {
  const auto now = std::chrono::steady_clock::now();
  EXPECT_TRUE(FreshCameraFrame(now - std::chrono::seconds(2), now));
  EXPECT_FALSE(FreshCameraFrame(now - std::chrono::seconds(2) - std::chrono::milliseconds(1), now));
  EXPECT_FALSE(FreshCameraFrame(now + std::chrono::milliseconds(1), now));
}

}  // namespace
}  // namespace eos::runtime::models

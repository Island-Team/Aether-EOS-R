#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "adapters/eos-cxx/runtime/init/vlm_image_asset.h"

namespace {

TEST(VlmImageAssetTest, ReadsOnlyBoundedJpegInsideAssetRoot) {
  const auto base = std::filesystem::temp_directory_path() / "eos-vlm-image-test";
  std::filesystem::remove_all(base);
  std::filesystem::create_directories(base / "assets");
  const auto write = [](const std::filesystem::path& path, std::string bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  };
  write(base / "assets" / "valid.jpg", "\xff\xd8" "payload" "\xff\xd9");
  write(base / "assets" / "bad.jpg", "not a jpeg");
  write(base / "outside.jpg", "\xff\xd8" "payload" "\xff\xd9");
  std::filesystem::create_symlink(base / "outside.jpg", base / "assets" / "escape.jpg");

  EXPECT_EQ(eos::runtime::init::ReadVlmJpegAsset(base / "assets", "asset://valid.jpg").size(), 11u);
  EXPECT_THROW(eos::runtime::init::ReadVlmJpegAsset(base / "assets", "asset://bad.jpg"),
               std::invalid_argument);
  EXPECT_THROW(eos::runtime::init::ReadVlmJpegAsset(base / "assets", "asset://../outside.jpg"),
               std::invalid_argument);
  EXPECT_THROW(eos::runtime::init::ReadVlmJpegAsset(base / "assets", "asset://escape.jpg"),
               std::invalid_argument);
  EXPECT_THROW(eos::runtime::init::ReadVlmJpegAsset(base / "assets", "asset:///valid.jpg"),
               std::invalid_argument);
  std::filesystem::remove_all(base);
}

}  // namespace

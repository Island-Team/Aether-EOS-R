#include "src/eos/runtime/init/rotated_frame.h"
#include "src/eos/runtime/models/camera_pose.h"

#include <jpeglib.h>
#include <gtest/gtest.h>

#include <cstdlib>

namespace {

std::vector<std::uint8_t> EncodeAsymmetricJpeg() {
  constexpr int kWidth = 32;
  constexpr int kHeight = 16;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * 3);
  for (int y = 0; y < kHeight; ++y)
    for (int x = 0; x < kWidth; ++x) {
      const auto offset = (y * kWidth + x) * 3;
      pixels[offset] = x < kWidth / 2 ? 240 : 10;
      pixels[offset + 1] = 5;
      pixels[offset + 2] = x < kWidth / 2 ? 10 : 240;
    }
  jpeg_compress_struct encoder{};
  jpeg_error_mgr error{};
  encoder.err = jpeg_std_error(&error);
  jpeg_create_compress(&encoder);
  unsigned char* output = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&encoder, &output, &size);
  encoder.image_width = kWidth;
  encoder.image_height = kHeight;
  encoder.input_components = 3;
  encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  jpeg_set_quality(&encoder, 100, TRUE);
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    JSAMPROW row = pixels.data() + encoder.next_scanline * kWidth * 3;
    jpeg_write_scanlines(&encoder, &row, 1);
  }
  jpeg_finish_compress(&encoder);
  std::vector<std::uint8_t> jpeg(output, output + size);
  jpeg_destroy_compress(&encoder);
  std::free(output);
  return jpeg;
}

std::pair<int, int> DecodeTopAndBottomRed(const std::vector<std::uint8_t>& jpeg) {
  jpeg_decompress_struct decoder{};
  jpeg_error_mgr error{};
  decoder.err = jpeg_std_error(&error);
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, jpeg.data(), jpeg.size());
  jpeg_read_header(&decoder, TRUE);
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  EXPECT_EQ(decoder.output_width, 16u);
  EXPECT_EQ(decoder.output_height, 32u);
  std::vector<std::uint8_t> pixels(decoder.output_width * decoder.output_height * 3);
  while (decoder.output_scanline < decoder.output_height) {
    JSAMPROW row = pixels.data() + decoder.output_scanline * decoder.output_width * 3;
    jpeg_read_scanlines(&decoder, &row, 1);
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  return {pixels[(3 * 16 + 8) * 3], pixels[(28 * 16 + 8) * 3]};
}

}  // namespace

namespace eos::runtime::init {
namespace {

TEST(RotatedFrame, JpegUsesRequestedDirectionAndPreservesMedia) {
  const auto source = EncodeAsymmetricJpeg();
  const auto ccw = DecodeTopAndBottomRed(RotateJpegQuarterTurn(source, 90));
  const auto cw = DecodeTopAndBottomRed(RotateJpegQuarterTurn(source, -90));
  EXPECT_LT(ccw.first, 40);
  EXPECT_GT(ccw.second, 200);
  EXPECT_GT(cw.first, 200);
  EXPECT_LT(cw.second, 40);
  EXPECT_THROW(RotateJpegQuarterTurn({0xff, 0xd8, 0xff}, 90), std::invalid_argument);
}

TEST(RotatedFrame, DepthAndPoseShareQuarterTurn) {
  const std::vector<std::uint16_t> source{1, 2, 3, 4, 5, 6};  // 3 x 2
  EXPECT_EQ(RotateDepthQuarterTurn(source, 3, 2, 90),
            (std::vector<std::uint16_t>{3, 6, 2, 5, 1, 4}));
  EXPECT_EQ(RotateDepthQuarterTurn(source, 3, 2, -90),
            (std::vector<std::uint16_t>{4, 1, 5, 2, 6, 3}));

  nlohmann::json clockwise{{"bbox", {0.0, 0.0, 2.0, 3.0}},
                            {"keypoints", {{0.5, 1.0, 0.9}}}};
  MapRotatedPersonToSource(clockwise, 3, 2, -90);
  EXPECT_EQ(clockwise.at("rotated_bbox"), nlohmann::json({0.0, 0.0, 2.0, 3.0}));
  EXPECT_EQ(clockwise.at("bbox"), nlohmann::json({0.0, 0.0, 3.0, 2.0}));
  EXPECT_EQ(clockwise.at("keypoints").at(0), nlohmann::json({1.0, 0.5, 0.9}));

  nlohmann::json counterclockwise{{"bbox", {0.0, 0.0, 2.0, 3.0}},
                                   {"keypoints", {{0.5, 1.0}}}};
  MapRotatedPersonToSource(counterclockwise, 3, 2, 90);
  EXPECT_EQ(counterclockwise.at("bbox"), nlohmann::json({0.0, 0.0, 3.0, 2.0}));
  EXPECT_EQ(counterclockwise.at("keypoints").at(0), nlohmann::json({1.0, 0.5}));

  nlohmann::json top_left{{"keypoints", {{0.0, 0.0}}}};
  MapRotatedPersonToSource(top_left, 3, 2, 90);
  EXPECT_EQ(top_left.at("keypoints").at(0), nlohmann::json({2.0, 0.0}));
  nlohmann::json other_top_left{{"keypoints", {{0.0, 0.0}}}};
  MapRotatedPersonToSource(other_top_left, 3, 2, -90);
  EXPECT_EQ(other_top_left.at("keypoints").at(0), nlohmann::json({0.0, 1.0}));
}

TEST(RotatedFrame, RejectsMalformedDepth) {
  EXPECT_THROW(RotateDepthQuarterTurn({1}, 3, 2, 90), std::invalid_argument);
  EXPECT_THROW(RotateDepthQuarterTurn({1, 2, 3, 4, 5, 6}, 3, 2, 0), std::invalid_argument);
}

TEST(RotatedFrame, DecodesAlignedPngWithoutChangingDepthUnits) {
  const std::vector<std::uint16_t> source{100, 200, 300, 400, 500, 600};
  const auto png = models::EncodeAlignedDepthPng(source, 3, 2);
  ASSERT_FALSE(png.empty());
  EXPECT_EQ(DecodeAlignedDepthPng(png, 3, 2), source);
  EXPECT_THROW(DecodeAlignedDepthPng(png, 2, 3), std::invalid_argument);
}

TEST(RotatedFrame, RotatesDepthIntrinsicsIntoSameCoordinateSystem) {
  hardware::CameraIntrinsics camera;
  camera.width_px = 640;
  camera.height_px = 480;
  camera.fx = 600;
  camera.fy = 610;
  camera.ppx = 321;
  camera.ppy = 239;
  camera.distortion_model = "brown_conrady";
  camera.distortion_coefficients = {0.1, 0.2, 0.03, 0.04, 0.5};
  const auto ccw = RotateDepthIntrinsics(camera, 90);
  EXPECT_EQ(ccw.at("width"), 480);
  EXPECT_EQ(ccw.at("height"), 640);
  EXPECT_EQ(ccw.at("fx"), 610);
  EXPECT_EQ(ccw.at("fy"), 600);
  EXPECT_EQ(ccw.at("ppx"), 239);
  EXPECT_EQ(ccw.at("ppy"), 318);
  EXPECT_EQ(ccw.at("coeffs").at(2), -0.04);
  EXPECT_EQ(ccw.at("coeffs").at(3), 0.03);
  const auto cw = RotateDepthIntrinsics(camera, -90);
  EXPECT_EQ(cw.at("ppx"), 240);
  EXPECT_EQ(cw.at("ppy"), 321);
  EXPECT_EQ(cw.at("coeffs").at(2), 0.04);
  EXPECT_EQ(cw.at("coeffs").at(3), -0.03);
}

}  // namespace
}  // namespace eos::runtime::init

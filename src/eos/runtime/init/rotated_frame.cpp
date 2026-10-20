#include "src/eos/runtime/init/rotated_frame.h"

#include <jpeglib.h>
#include <png.h>

#include <algorithm>
#include <csetjmp>
#include <cstdlib>
#include <stdexcept>

namespace eos::runtime::init {
namespace {

struct JpegError {
  jpeg_error_mgr base;
  jmp_buf jump;
};

void FailJpeg(j_common_ptr info) {
  auto* error = reinterpret_cast<JpegError*>(info->err);
  longjmp(error->jump, 1);
}

template <typename T>
std::vector<T> RotatePixels(const std::vector<T>& pixels, std::uint32_t width,
                            std::uint32_t height, std::size_t channels, int degrees) {
  if ((degrees != -90 && degrees != 90) || width == 0 || height == 0 ||
      pixels.size() != static_cast<std::size_t>(width) * height * channels)
    throw std::invalid_argument("quarter-turn image has invalid dimensions or rotation");
  std::vector<T> output(pixels.size());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const std::uint32_t rotated_x = degrees == 90 ? y : height - 1 - y;
      const std::uint32_t rotated_y = degrees == 90 ? width - 1 - x : x;
      const auto source = (static_cast<std::size_t>(y) * width + x) * channels;
      const auto target = (static_cast<std::size_t>(rotated_y) * height + rotated_x) * channels;
      std::copy_n(pixels.begin() + source, channels, output.begin() + target);
    }
  }
  return output;
}

std::pair<double, double> ToSource(double x, double y, std::uint32_t width,
                                   std::uint32_t height, int degrees) {
  if (degrees == 90) return {static_cast<double>(width) - y, x};
  return {y, static_cast<double>(height) - x};
}

std::pair<double, double> ToSourcePixel(double x, double y, std::uint32_t width,
                                        std::uint32_t height, int degrees) {
  if (degrees == 90) return {static_cast<double>(width - 1) - y, x};
  return {y, static_cast<double>(height - 1) - x};
}

}  // namespace

std::vector<std::uint8_t> RotateJpegQuarterTurn(const std::vector<std::uint8_t>& jpeg,
                                                 int degrees) {
  if (jpeg.empty() || (degrees != -90 && degrees != 90))
    throw std::invalid_argument("invalid JPEG quarter-turn input");
  jpeg_decompress_struct decoder{};
  JpegError decode_error{};
  decoder.err = jpeg_std_error(&decode_error.base);
  decode_error.base.error_exit = FailJpeg;
  if (setjmp(decode_error.jump)) {
    jpeg_destroy_decompress(&decoder);
    throw std::invalid_argument("invalid retained JPEG frame");
  }
  jpeg_create_decompress(&decoder);
  jpeg_mem_src(&decoder, jpeg.data(), jpeg.size());
  jpeg_read_header(&decoder, TRUE);
  decoder.out_color_space = JCS_RGB;
  jpeg_start_decompress(&decoder);
  const auto width = decoder.output_width;
  const auto height = decoder.output_height;
  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
  while (decoder.output_scanline < height) {
    JSAMPROW row = rgb.data() + static_cast<std::size_t>(decoder.output_scanline) * width * 3;
    jpeg_read_scanlines(&decoder, &row, 1);
  }
  jpeg_finish_decompress(&decoder);
  jpeg_destroy_decompress(&decoder);
  auto rotated = RotatePixels(rgb, width, height, 3, degrees);

  jpeg_compress_struct encoder{};
  JpegError encode_error{};
  encoder.err = jpeg_std_error(&encode_error.base);
  encode_error.base.error_exit = FailJpeg;
  unsigned char* encoded = nullptr;
  unsigned long encoded_size = 0;
  if (setjmp(encode_error.jump)) {
    jpeg_destroy_compress(&encoder);
    std::free(encoded);
    throw std::runtime_error("failed to encode rotated JPEG");
  }
  jpeg_create_compress(&encoder);
  jpeg_mem_dest(&encoder, &encoded, &encoded_size);
  encoder.image_width = height;
  encoder.image_height = width;
  encoder.input_components = 3;
  encoder.in_color_space = JCS_RGB;
  jpeg_set_defaults(&encoder);
  jpeg_set_quality(&encoder, 90, TRUE);
  jpeg_start_compress(&encoder, TRUE);
  while (encoder.next_scanline < encoder.image_height) {
    JSAMPROW row = rotated.data() +
                   static_cast<std::size_t>(encoder.next_scanline) * encoder.image_width * 3;
    jpeg_write_scanlines(&encoder, &row, 1);
  }
  jpeg_finish_compress(&encoder);
  std::vector<std::uint8_t> output(encoded, encoded + encoded_size);
  jpeg_destroy_compress(&encoder);
  std::free(encoded);
  return output;
}

std::vector<std::uint16_t> RotateDepthQuarterTurn(const std::vector<std::uint16_t>& depth,
                                                  std::uint32_t width, std::uint32_t height,
                                                  int degrees) {
  return RotatePixels(depth, width, height, 1, degrees);
}

std::vector<std::uint16_t> DecodeAlignedDepthPng(const std::vector<std::uint8_t>& png,
                                                  std::uint32_t width, std::uint32_t height) {
  if (png.empty() || width == 0 || height == 0)
    throw std::invalid_argument("invalid aligned depth PNG");
  png_image image{};
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&image, png.data(), png.size()))
    throw std::invalid_argument("invalid aligned depth PNG");
  if (image.width != width || image.height != height) {
    png_image_free(&image);
    throw std::invalid_argument("aligned depth PNG dimensions differ from RGB frame");
  }
  image.format = PNG_FORMAT_LINEAR_Y;
  std::vector<std::uint16_t> pixels(static_cast<std::size_t>(width) * height);
  if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
    png_image_free(&image);
    throw std::invalid_argument("failed to decode aligned depth PNG");
  }
  png_image_free(&image);
  return pixels;
}

void MapRotatedPersonToSource(nlohmann::json& person, std::uint32_t source_width,
                              std::uint32_t source_height, int degrees) {
  if (!person.is_object() || (degrees != -90 && degrees != 90)) return;
  person.erase("bearing_rad");
  if (person.contains("bbox") && person.at("bbox").is_array() &&
      person.at("bbox").size() == 4) {
    const auto box = person.at("bbox");
    const auto a = ToSource(box.at(0).get<double>(), box.at(1).get<double>(),
                            source_width, source_height, degrees);
    const auto b = ToSource(box.at(2).get<double>(), box.at(3).get<double>(),
                            source_width, source_height, degrees);
    person["rotated_bbox"] = box;
    person["bbox"] = {std::clamp(std::min(a.first, b.first), 0.0, double(source_width)),
                      std::clamp(std::min(a.second, b.second), 0.0, double(source_height)),
                      std::clamp(std::max(a.first, b.first), 0.0, double(source_width)),
                      std::clamp(std::max(a.second, b.second), 0.0, double(source_height))};
  }
  if (person.contains("keypoints") && person.at("keypoints").is_array()) {
    for (auto& point : person["keypoints"]) {
      if (!point.is_array() || point.size() < 2 || !point.at(0).is_number() ||
          !point.at(1).is_number()) continue;
      const auto mapped = ToSourcePixel(point.at(0).get<double>(), point.at(1).get<double>(),
                                        source_width, source_height, degrees);
      point[0] = std::clamp(mapped.first, 0.0, double(source_width - 1));
      point[1] = std::clamp(mapped.second, 0.0, double(source_height - 1));
    }
  }
}

nlohmann::json RotateDepthIntrinsics(const hardware::CameraIntrinsics& camera, int degrees) {
  if ((degrees != -90 && degrees != 90) || camera.width_px == 0 || camera.height_px == 0)
    throw std::invalid_argument("invalid depth intrinsics quarter-turn");
  auto coefficients = camera.distortion_coefficients;
  if (degrees == 90) {
    coefficients[2] = -camera.distortion_coefficients[3];
    coefficients[3] = camera.distortion_coefficients[2];
  } else {
    coefficients[2] = camera.distortion_coefficients[3];
    coefficients[3] = -camera.distortion_coefficients[2];
  }
  return {{"width", camera.height_px}, {"height", camera.width_px},
          {"fx", camera.fy}, {"fy", camera.fx},
          {"ppx", degrees == 90 ? camera.ppy : double(camera.height_px - 1) - camera.ppy},
          {"ppy", degrees == 90 ? double(camera.width_px - 1) - camera.ppx : camera.ppx},
          {"distortion_model", camera.distortion_model}, {"coeffs", coefficients}};
}

}  // namespace eos::runtime::init

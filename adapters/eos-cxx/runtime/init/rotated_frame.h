#pragma once

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>
#include "eos/hardware/backend.h"

namespace eos::runtime::init {

// Positive degrees rotate counterclockwise. Inputs are complete JPEG frames.
std::vector<std::uint8_t> RotateJpegQuarterTurn(const std::vector<std::uint8_t>& jpeg,
                                                 int degrees);
std::vector<std::uint16_t> RotateDepthQuarterTurn(const std::vector<std::uint16_t>& depth,
                                                  std::uint32_t width, std::uint32_t height,
                                                  int degrees);
std::vector<std::uint16_t> DecodeAlignedDepthPng(const std::vector<std::uint8_t>& png,
                                                  std::uint32_t width, std::uint32_t height);
void MapRotatedPersonToSource(nlohmann::json& person, std::uint32_t source_width,
                              std::uint32_t source_height, int degrees);
nlohmann::json RotateDepthIntrinsics(const hardware::CameraIntrinsics& camera, int degrees);

}  // namespace eos::runtime::init

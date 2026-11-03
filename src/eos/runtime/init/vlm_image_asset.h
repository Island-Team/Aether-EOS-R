#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace eos::runtime::init {

// Resolve one JPEG asset under the EOS state assets directory. Throws on invalid input.
std::vector<std::uint8_t> ReadVlmJpegAsset(const std::filesystem::path& root,
                                           const std::string& uri);

}  // namespace eos::runtime::init

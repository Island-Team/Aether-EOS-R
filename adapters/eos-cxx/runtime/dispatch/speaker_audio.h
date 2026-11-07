#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace eos::runtime::dispatch::detail {

bool IsPcmWave(const std::vector<std::uint8_t>& bytes);

// Converts a bounded PCM16 WAV to the speaker's canonical 16 kHz mono format.
std::optional<std::vector<std::uint8_t>> NormalizePcmWave(
    const std::vector<std::uint8_t>& bytes);

}  // namespace eos::runtime::dispatch::detail

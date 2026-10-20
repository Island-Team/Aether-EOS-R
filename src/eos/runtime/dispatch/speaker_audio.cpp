#include "speaker_audio.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace eos::runtime::dispatch::detail {
namespace {

constexpr std::size_t kMaximumAudioBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kOutputRate = 16000;

std::uint16_t ReadLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t ReadLe32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         static_cast<std::uint32_t>(bytes[offset + 1]) << 8 |
         static_cast<std::uint32_t>(bytes[offset + 2]) << 16 |
         static_cast<std::uint32_t>(bytes[offset + 3]) << 24;
}

void WriteLe16(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint16_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void WriteLe32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

struct PcmWave {
  std::uint16_t channels{0};
  std::uint32_t sample_rate{0};
  std::size_t data_offset{0};
  std::size_t data_size{0};
};

std::optional<PcmWave> ParsePcm16Wave(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < 12 || bytes.size() > kMaximumAudioBytes ||
      std::string(bytes.begin(), bytes.begin() + 4) != "RIFF" ||
      std::string(bytes.begin() + 8, bytes.begin() + 12) != "WAVE" ||
      static_cast<std::size_t>(ReadLe32(bytes, 4)) + 8 != bytes.size()) {
    return std::nullopt;
  }

  PcmWave wave;
  bool format = false;
  for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
    const std::string id(bytes.begin() + offset, bytes.begin() + offset + 4);
    const std::size_t size = ReadLe32(bytes, offset + 4);
    const std::size_t data = offset + 8;
    if (size > bytes.size() - data) return std::nullopt;
    if (id == "fmt ") {
      if (size < 16 || ReadLe16(bytes, data) != 1 || ReadLe16(bytes, data + 14) != 16) {
        return std::nullopt;
      }
      wave.channels = ReadLe16(bytes, data + 2);
      wave.sample_rate = ReadLe32(bytes, data + 4);
      if (wave.channels == 0 || wave.channels > 2 || wave.sample_rate == 0 ||
          ReadLe16(bytes, data + 12) != wave.channels * 2 ||
          ReadLe32(bytes, data + 8) !=
              static_cast<std::uint64_t>(wave.sample_rate) * wave.channels * 2) {
        return std::nullopt;
      }
      format = true;
    } else if (id == "data") {
      if (!format || size == 0 || size % (wave.channels * 2) != 0) return std::nullopt;
      wave.data_offset = data;
      wave.data_size = size;
      return wave;
    }
    const std::size_t padding = size % 2;
    if (data + size + padding > bytes.size()) return std::nullopt;
    offset = data + size + padding;
  }
  return std::nullopt;
}

std::int16_t MonoSample(const std::vector<std::uint8_t>& bytes, const PcmWave& wave,
                        std::size_t frame) {
  const std::size_t offset = wave.data_offset + frame * wave.channels * 2;
  const auto left = static_cast<std::int16_t>(ReadLe16(bytes, offset));
  if (wave.channels == 1) return left;
  const auto right = static_cast<std::int16_t>(ReadLe16(bytes, offset + 2));
  return static_cast<std::int16_t>((static_cast<std::int32_t>(left) + right) / 2);
}

}  // namespace

bool IsPcmWave(const std::vector<std::uint8_t>& bytes) {
  const auto wave = ParsePcm16Wave(bytes);
  return wave && wave->channels == 1 && wave->sample_rate == kOutputRate;
}

std::optional<std::vector<std::uint8_t>> NormalizePcmWave(
    const std::vector<std::uint8_t>& bytes) {
  const auto wave = ParsePcm16Wave(bytes);
  if (!wave) return std::nullopt;
  if (wave->channels == 1 && wave->sample_rate == kOutputRate) return bytes;

  const std::size_t input_frames = wave->data_size / (wave->channels * 2);
  const std::uint64_t scaled_frames =
      static_cast<std::uint64_t>(input_frames) * kOutputRate + wave->sample_rate / 2;
  const std::size_t output_frames =
      std::max<std::size_t>(1, static_cast<std::size_t>(scaled_frames / wave->sample_rate));
  if (output_frames > (kMaximumAudioBytes - 44) / 2 ||
      output_frames > std::numeric_limits<std::uint32_t>::max() / 2) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> output(44 + output_frames * 2);
  std::copy_n("RIFF", 4, output.begin());
  WriteLe32(&output, 4, static_cast<std::uint32_t>(output.size() - 8));
  std::copy_n("WAVEfmt ", 8, output.begin() + 8);
  WriteLe32(&output, 16, 16);
  WriteLe16(&output, 20, 1);
  WriteLe16(&output, 22, 1);
  WriteLe32(&output, 24, kOutputRate);
  WriteLe32(&output, 28, kOutputRate * 2);
  WriteLe16(&output, 32, 2);
  WriteLe16(&output, 34, 16);
  std::copy_n("data", 4, output.begin() + 36);
  WriteLe32(&output, 40, static_cast<std::uint32_t>(output_frames * 2));

  for (std::size_t frame = 0; frame < output_frames; ++frame) {
    const double source = static_cast<double>(frame) * wave->sample_rate / kOutputRate;
    const std::size_t lower = std::min(static_cast<std::size_t>(source), input_frames - 1);
    const std::size_t upper = std::min(lower + 1, input_frames - 1);
    const double fraction = source - static_cast<double>(lower);
    const double sample = MonoSample(bytes, *wave, lower) * (1.0 - fraction) +
                          MonoSample(bytes, *wave, upper) * fraction;
    const auto value = static_cast<std::int16_t>(std::clamp(
        std::lround(sample), static_cast<long>(std::numeric_limits<std::int16_t>::min()),
        static_cast<long>(std::numeric_limits<std::int16_t>::max())));
    WriteLe16(&output, 44 + frame * 2, static_cast<std::uint16_t>(value));
  }
  return output;
}

}  // namespace eos::runtime::dispatch::detail

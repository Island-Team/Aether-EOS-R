#include "uuid.h"

#include <array>
#include <cstdio>
#include <random>

namespace eos::lib {

std::string EncodeUuidV7(std::chrono::milliseconds unix_time, std::uint64_t random_hi,
                         std::uint64_t random_lo) {
  const std::uint64_t timestamp = static_cast<std::uint64_t>(unix_time.count()) & 0xffffffffffffULL;
  const std::uint16_t rand_a = static_cast<std::uint16_t>(random_hi & 0x0fffU);
  const std::uint64_t rand_b = random_lo & 0x3fffffffffffffffULL;
  const std::uint16_t group4 = static_cast<std::uint16_t>((rand_b >> 48U) | 0x8000U);
  const std::uint64_t group5 = rand_b & 0xffffffffffffULL;

  std::array<char, 37> text{};
  std::snprintf(text.data(), text.size(), "%08llx-%04llx-7%03x-%04x-%012llx",
                static_cast<unsigned long long>(timestamp >> 16U),
                static_cast<unsigned long long>(timestamp & 0xffffU), rand_a, group4,
                static_cast<unsigned long long>(group5));
  return text.data();
}

std::string MintUuidV7() {
  std::random_device source;
  std::mt19937_64 random(source());
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return EncodeUuidV7(now, random(), random());
}

}  // namespace eos::lib

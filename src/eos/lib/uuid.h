/// UUID helpers used for process-scoped runtime identity.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace eos::lib {

/// Mint a lowercase RFC 9562 UUIDv7 using the current Unix epoch milliseconds.
std::string MintUuidV7();

/// Testable UUIDv7 encoder. `random_hi` and `random_lo` supply the 74 random
/// bits after the version and variant fields.
std::string EncodeUuidV7(std::chrono::milliseconds unix_time, std::uint64_t random_hi,
                         std::uint64_t random_lo);

}  // namespace eos::lib

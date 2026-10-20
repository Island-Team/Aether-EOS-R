#include "src/eos/lib/uuid.h"

#include <gtest/gtest.h>

#include <chrono>
#include <regex>

namespace eos::lib {
namespace {

TEST(UuidTest, EncodesCanonicalUuidV7VersionVariantAndTimestamp) {
  const std::string uuid = EncodeUuidV7(std::chrono::milliseconds(0x0123456789abULL),
                                        0x0000000000000defULL, 0x123456789abcdef0ULL);
  EXPECT_EQ(uuid, "01234567-89ab-7def-9234-56789abcdef0");
  EXPECT_TRUE(std::regex_match(
      uuid, std::regex("[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")));
}

TEST(UuidTest, MintedRuntimeIdsAreDistinct) {
  EXPECT_NE(MintUuidV7(), MintUuidV7());
}

}  // namespace
}  // namespace eos::lib

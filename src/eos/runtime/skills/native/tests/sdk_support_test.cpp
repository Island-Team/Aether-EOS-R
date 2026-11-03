#include <eos/skill/support.h>

#include "gtest/gtest.h"

TEST(SkillSdkSupportTest, ChecksVersionAndLibraryLayout) {
  EXPECT_EQ(eos_skill_sdk_abi_version(), EOS_SKILL_ABI_VERSION_V2);
  EXPECT_EQ(eos_skill_sdk_check_library_v2(nullptr), EOS_SKILL_SDK_INVALID_ARGUMENT_V2);

  eos_skill_descriptor_v2 descriptor{};
  descriptor.struct_size = sizeof(descriptor);
  eos_skill_library_v2 library{};
  library.abi_version = EOS_SKILL_ABI_VERSION_V2;
  library.struct_size = sizeof(library);
  library.skill_count = 1;
  library.skills = &descriptor;
  library.skill_descriptor_size = sizeof(descriptor);
  EXPECT_EQ(eos_skill_sdk_check_library_v2(&library), EOS_SKILL_SDK_COMPATIBLE_V2);

  library.abi_version = 99;
  EXPECT_EQ(eos_skill_sdk_check_library_v2(&library), EOS_SKILL_SDK_INCOMPATIBLE_ABI_V2);
  library.abi_version = EOS_SKILL_ABI_VERSION_V2;
  library.skill_descriptor_size = EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE - 1;
  EXPECT_EQ(eos_skill_sdk_check_library_v2(&library), EOS_SKILL_SDK_INVALID_SKILL_TABLE_V2);
}

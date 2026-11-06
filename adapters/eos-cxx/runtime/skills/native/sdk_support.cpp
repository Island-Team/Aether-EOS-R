#include <eos/skill/support.h>

#include <cstddef>

extern "C" uint32_t eos_skill_sdk_abi_version(void) { return EOS_SKILL_ABI_VERSION_V2; }

extern "C" eos_skill_sdk_check_v2 eos_skill_sdk_check_library_v2(
    const eos_skill_library_v2* library) {
  if (library == nullptr) return EOS_SKILL_SDK_INVALID_ARGUMENT_V2;
  if (library->abi_version != EOS_SKILL_ABI_VERSION_V2) return EOS_SKILL_SDK_INCOMPATIBLE_ABI_V2;
  if (library->struct_size < EOS_SKILL_LIBRARY_V2_BASE_SIZE)
    return EOS_SKILL_SDK_INVALID_LIBRARY_LAYOUT_V2;
  if (library->skill_count == 0 || library->skill_count > 256 || library->skills == nullptr)
    return EOS_SKILL_SDK_INVALID_SKILL_TABLE_V2;
  const uint32_t descriptor_size = EOS_SKILL_LIBRARY_V2_HAS(library, skill_descriptor_size)
                                       ? library->skill_descriptor_size
                                       : EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE;
  if (descriptor_size < EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE ||
      descriptor_size % alignof(eos_skill_descriptor_v2) != 0)
    return EOS_SKILL_SDK_INVALID_SKILL_TABLE_V2;
  return EOS_SKILL_SDK_COMPATIBLE_V2;
}

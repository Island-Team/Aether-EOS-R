/// Small shared helpers for authors of native EOS skill libraries.
/// The EOS loader remains the authority for schemas, names, and capabilities.
#ifndef EOS_SKILL_SUPPORT_H_
#define EOS_SKILL_SUPPORT_H_

#include <eos/skill/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t eos_skill_sdk_check_v2;
#define EOS_SKILL_SDK_COMPATIBLE_V2 0u
#define EOS_SKILL_SDK_INVALID_ARGUMENT_V2 1u
#define EOS_SKILL_SDK_INCOMPATIBLE_ABI_V2 2u
#define EOS_SKILL_SDK_INVALID_LIBRARY_LAYOUT_V2 3u
#define EOS_SKILL_SDK_INVALID_SKILL_TABLE_V2 4u

/// Reports the native skill ABI supported by this shared SDK.
EOS_SKILL_EXPORT uint32_t eos_skill_sdk_abi_version(void);

/// Checks the fixed header and table layout without dereferencing descriptors.
/// Call only with a pointer to a readable `eos_skill_library_v2` in this process.
EOS_SKILL_EXPORT eos_skill_sdk_check_v2
eos_skill_sdk_check_library_v2(const eos_skill_library_v2* library);

#ifdef __cplusplus
}
#endif

#endif  // EOS_SKILL_SUPPORT_H_

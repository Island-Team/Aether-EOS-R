/// Test-only library advertising an unsupported ABI version.
#include "eos/skill/abi.h"

namespace {
const eos_skill_library_v2 kLibrary{
    EOS_SKILL_ABI_VERSION_V2 - 1, sizeof(eos_skill_library_v2), {}, {}, {}, 0, nullptr};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t) {
  return &kLibrary;
}

/// Test-only library with a descriptor stride that cannot preserve alignment.
#include "eos/skill/abi.h"

namespace {
eos_skill_execute_status_v2 Execute(void*, const eos_skill_execution_v2*, eos_skill_complete_v2,
                                    void*) {
  return EOS_SKILL_EXECUTE_FAILED;
}

const char kSchemaJson[] = R"({"type":"object"})";
const eos_skill_text_v2 kSchema = {kSchemaJson, sizeof(kSchemaJson) - 1};
const eos_skill_descriptor_v2 kSkills[] = {{sizeof(eos_skill_descriptor_v2),
                                            {"fixture.bad_stride", 18},
                                            {"1.0.0", 5},
                                            {},
                                            kSchema,
                                            kSchema,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            Execute}};
const eos_skill_library_v2 kLibrary = {
    EOS_SKILL_ABI_VERSION_V2, sizeof(eos_skill_library_v2), {"fixture", 7}, {"1.0.0", 5}, {},
    1, kSkills, static_cast<uint32_t>(sizeof(eos_skill_descriptor_v2) + 1)};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t) {
  return &kLibrary;
}

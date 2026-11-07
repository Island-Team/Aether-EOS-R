/// Test-only library whose embedded primitive allowlist cannot be admitted.
#include "eos/skill/abi.h"

namespace {
eos_skill_execute_status_v2 Execute(void*, const eos_skill_execution_v2*, eos_skill_complete_v2,
                                    void*) {
  return EOS_SKILL_EXECUTE_FAILED;
}

const eos_skill_primitive_reference_v2 kPrimitives[] = {
    {sizeof(eos_skill_primitive_reference_v2), {"invented.action", 15}, {"1.0.0", 5}}};
const char kSchemaJson[] =
    R"({"$schema":"https://json-schema.org/draft/2020-12/schema","type":"object"})";
const eos_skill_text_v2 kSchema = {kSchemaJson, sizeof(kSchemaJson) - 1};
const eos_skill_descriptor_v2 kSkills[] = {{sizeof(eos_skill_descriptor_v2),
                                            {"fixture.unknown", 15},
                                            {"1.0.0", 5},
                                            {},
                                            kSchema,
                                            kSchema,
                                            1,
                                            kPrimitives,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            0,
                                            nullptr,
                                            Execute}};
const eos_skill_library_v2 kLibrary = {EOS_SKILL_ABI_VERSION_V2,
                                       sizeof(eos_skill_library_v2),
                                       {"fixture", 7},
                                       {"1.0.0", 5},
                                       {},
                                       1,
                                        kSkills,
                                        sizeof(eos_skill_descriptor_v2)};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t) {
  return &kLibrary;
}

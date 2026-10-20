#include <cstddef>

#include "eos/skill/abi.h"

namespace {
template <std::size_t Size>
constexpr eos_skill_text_v2 Text(const char (&value)[Size]) {
  return {value, Size - 1};
}

eos_skill_execute_status_v2 Execute(void*, const eos_skill_execution_v2*,
                                    eos_skill_complete_v2 complete, void* context) {
  const eos_skill_completion_v2 result{sizeof(result), 1, Text("completed"), Text("{}")};
  return complete(context, &result) == EOS_SKILL_HOST_OK ? EOS_SKILL_EXECUTE_OK
                                                          : EOS_SKILL_EXECUTE_FAILED;
}

// This is the original descriptor layout, deliberately excluding all appended fields.
struct OldDescriptorV2 {
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 description;
  eos_skill_text_v2 parameters_schema_json;
  eos_skill_text_v2 returns_schema_json;
  uint32_t primitive_count;
  const eos_skill_primitive_reference_v2* primitives;
  uint32_t resource_count;
  const eos_skill_resource_v2* resources;
  uint32_t signal_count;
  const eos_skill_signal_descriptor_v2* signals;
  uint32_t claim_count;
  const eos_skill_claim_template_v2* claims;
  uint32_t reentrant;
  void* context;
  eos_skill_execute_v2 execute;
};
static_assert(sizeof(OldDescriptorV2) == EOS_SKILL_DESCRIPTOR_V2_BASE_SIZE);

constexpr char kSchema[] = R"({"type":"object"})";
const OldDescriptorV2 kSkills[] = {
    {sizeof(OldDescriptorV2), Text("fixture.old_first"), Text("1.0.0"), Text("old first"),
     Text(kSchema), Text(kSchema), 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr,
     Execute},
    {sizeof(OldDescriptorV2), Text("fixture.old_second"), Text("1.0.0"), Text("old second"),
     Text(kSchema), Text(kSchema), 0, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 1, nullptr,
     Execute},
};

// The old library header ends at `skills`, so it cannot advertise a descriptor stride.
struct OldLibraryV2 {
  uint32_t abi_version;
  uint32_t struct_size;
  eos_skill_text_v2 name;
  eos_skill_text_v2 version;
  eos_skill_text_v2 build_id;
  uint32_t skill_count;
  const eos_skill_descriptor_v2* skills;
};
static_assert(sizeof(OldLibraryV2) == EOS_SKILL_LIBRARY_V2_BASE_SIZE);
const OldLibraryV2 kLibrary{EOS_SKILL_ABI_VERSION_V2,
                            sizeof(OldLibraryV2),
                            Text("fixture.v2.old"),
                            Text("1.0.0"),
                            Text("old-sdk"),
                            2,
                            reinterpret_cast<const eos_skill_descriptor_v2*>(kSkills)};
}  // namespace

extern "C" EOS_SKILL_EXPORT const eos_skill_library_v2* eos_skill_library_query_v2(uint32_t abi) {
  return abi == EOS_SKILL_ABI_VERSION_V2
             ? reinterpret_cast<const eos_skill_library_v2*>(&kLibrary)
             : nullptr;
}

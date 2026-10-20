/// Test-only legacy library that exports only the removed ABI v1 query symbol.
#include <cstdint>

extern "C" __attribute__((visibility("default"))) const void* eos_skill_library_query(uint32_t) {
  return nullptr;
}

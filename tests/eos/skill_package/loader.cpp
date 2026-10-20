#include <dlfcn.h>

#include <eos/skill/abi.h>

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (module == nullptr) return 3;
  auto query = reinterpret_cast<eos_skill_library_query_v2_fn>(
      dlsym(module, EOS_SKILL_QUERY_SYMBOL_V2));
  const eos_skill_library_v2* library =
      query == nullptr ? nullptr : query(EOS_SKILL_ABI_VERSION_V2);
  const bool valid = library != nullptr && library->skill_count == 1 &&
                     query(EOS_SKILL_ABI_VERSION_V2 + 1) == nullptr;
  dlclose(module);
  return valid ? 0 : 4;
}

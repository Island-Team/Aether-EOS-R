# Copy this directory to the ignored share/eos/skills/native/ directory.
add_library(island_native_skills skills.cpp)
target_compile_features(island_native_skills PUBLIC cxx_std_20)
target_link_libraries(island_native_skills
  PUBLIC eos_internal eos_skills_builtin eos_skills_core nlohmann_json::nlohmann_json
)

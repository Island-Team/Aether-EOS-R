add_library(eos_skills_builtin
  registry.cpp
)

target_compile_features(eos_skills_builtin PUBLIC cxx_std_20)
target_link_libraries(eos_skills_builtin
  PUBLIC eos_internal eos_lib eos_skills_core nlohmann_json::nlohmann_json
  PRIVATE nlohmann_json_schema_validator
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_library(eos_skills_native
  executor.cpp
  loader.cpp
  registry.cpp
)

target_link_libraries(eos_skills_native
  PUBLIC eos_dispatch eos_internal eos_skills_core nlohmann_json::nlohmann_json
  PRIVATE ${CMAKE_DL_LIBS} eos_lib eos_skill_api eos_skills_logging
          nlohmann_json_schema_validator
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

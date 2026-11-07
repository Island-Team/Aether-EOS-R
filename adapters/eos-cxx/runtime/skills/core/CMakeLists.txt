add_library(eos_skills_core
  admission.cpp
  catalog.cpp
  model.cpp
)

target_link_libraries(eos_skills_core
  PUBLIC eos_internal nlohmann_json::nlohmann_json nlohmann_json_schema_validator
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

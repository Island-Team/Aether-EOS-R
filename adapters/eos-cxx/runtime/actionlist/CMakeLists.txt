add_library(eos_actionlist journal.cpp manager.cpp)
target_link_libraries(eos_actionlist
  PUBLIC eos_skills nlohmann_json::nlohmann_json
  PRIVATE eos_lib
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

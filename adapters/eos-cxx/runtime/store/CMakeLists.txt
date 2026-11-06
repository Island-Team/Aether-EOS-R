add_library(eos_navigation_store navigation_point_store.cpp furniture_region_store.cpp)
target_link_libraries(eos_navigation_store PUBLIC eos_internal nlohmann_json::nlohmann_json)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

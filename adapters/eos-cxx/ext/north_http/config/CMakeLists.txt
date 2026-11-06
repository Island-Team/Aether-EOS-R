add_library(eos_north_http_config config.cpp)
target_include_directories(eos_north_http_config PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_north_http_config PUBLIC eos_lib PRIVATE tomlplusplus::tomlplusplus)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

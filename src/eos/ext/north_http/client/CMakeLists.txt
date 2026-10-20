add_library(eos_north_http_client client.cpp)
target_include_directories(eos_north_http_client PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_north_http_client PUBLIC island_runtime_proto)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

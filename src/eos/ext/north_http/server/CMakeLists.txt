find_package(Threads REQUIRED)

add_library(eos_north_http_server server.cpp)
target_include_directories(eos_north_http_server PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_north_http_server
  PUBLIC eos_north_http_audit eos_north_http_client eos_north_http_config
  PRIVATE eos_lib island_runtime_proto Boost::boost Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

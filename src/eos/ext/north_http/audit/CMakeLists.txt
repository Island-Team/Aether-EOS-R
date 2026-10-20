find_package(Threads REQUIRED)

add_library(eos_north_http_audit audit.cpp)
target_include_directories(eos_north_http_audit PUBLIC "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_north_http_audit
  PUBLIC eos_north_http_client nlohmann_json::nlohmann_json
  PRIVATE eos_lib island_runtime_proto OpenSSL::Crypto Threads::Threads
)

if(ISLAND_BUILD_TESTS)
  add_subdirectory(tests)
endif()

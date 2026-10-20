add_subdirectory(config)
add_subdirectory(client)
add_subdirectory(audit)
add_subdirectory(server)

add_executable(eos_north_http main.cpp)
set_target_properties(eos_north_http PROPERTIES OUTPUT_NAME eos-north-http)
target_include_directories(eos_north_http PRIVATE "${ISLAND_SOURCE_DIR}")
target_link_libraries(eos_north_http PRIVATE
  eos_north_http_audit eos_north_http_client eos_north_http_config eos_north_http_server eos_lib
)

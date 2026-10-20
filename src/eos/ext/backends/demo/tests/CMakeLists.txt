# Shared demo core structure test.
add_executable(demo_core_test
  core_test.cpp
)

target_include_directories(demo_core_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_core_test
  PRIVATE eos_backend_demo_core eos_internal
)

add_test(NAME demo_core_test COMMAND demo_core_test)

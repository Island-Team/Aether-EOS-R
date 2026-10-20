# Demo base backend structure test — stub data through the south-bound
# interface.
add_executable(demo_base_test
  demo_base_test.cpp
)

target_include_directories(demo_base_test
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(demo_base_test
  PRIVATE eos_backend_demo_base eos_internal
)

add_test(NAME demo_base_test COMMAND demo_base_test)
